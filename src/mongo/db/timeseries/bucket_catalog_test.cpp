/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace {
class BucketCatalogTest : public CatalogTestFixture {
protected:
    void setUp() override;
    virtual BSONObj _makeTimeseriesOptionsForCreate() const;

    void _commit(const BucketCatalog::BucketId& bucketId, uint16_t numCommittedMeasurements);
    void _insertOneAndCommit(const NamespaceString& ns, uint16_t numCommittedMeasurements);

    OperationContext* _opCtx;
    BucketCatalog* _bucketCatalog;

    StringData _timeField = "time";
    StringData _metaField = "meta";

    NamespaceString _ns1{"bucket_catalog_test_1", "t_1"};
    NamespaceString _ns2{"bucket_catalog_test_1", "t_2"};
    NamespaceString _ns3{"bucket_catalog_test_2", "t_1"};

    BucketCatalog::CommitInfo _commitInfo{StatusWith<SingleWriteResult>(SingleWriteResult{})};
};

class BucketCatalogWithoutMetadataTest : public BucketCatalogTest {
protected:
    BSONObj _makeTimeseriesOptionsForCreate() const override;
};

void BucketCatalogTest::setUp() {
    CatalogTestFixture::setUp();

    _opCtx = operationContext();
    _bucketCatalog = &BucketCatalog::get(_opCtx);

    for (const auto& ns : {_ns1, _ns2, _ns3}) {
        ASSERT_OK(createCollection(
            _opCtx,
            ns.db().toString(),
            BSON("create" << ns.coll() << "timeseries" << _makeTimeseriesOptionsForCreate())));
    }
}

BSONObj BucketCatalogTest::_makeTimeseriesOptionsForCreate() const {
    return BSON("timeField" << _timeField << "metaField" << _metaField);
}

BSONObj BucketCatalogWithoutMetadataTest::_makeTimeseriesOptionsForCreate() const {
    return BSON("timeField" << _timeField);
}

void BucketCatalogTest::_commit(const BucketCatalog::BucketId& bucketId,
                                uint16_t numCommittedMeasurements) {
    auto data = _bucketCatalog->commit(bucketId);
    ASSERT_EQ(data.docs.size(), 1);
    ASSERT_EQ(data.numCommittedMeasurements, numCommittedMeasurements);

    data = _bucketCatalog->commit(bucketId, _commitInfo);
    ASSERT_EQ(data.docs.size(), 0);
    ASSERT_EQ(data.numCommittedMeasurements, numCommittedMeasurements + 1);
}

void BucketCatalogTest::_insertOneAndCommit(const NamespaceString& ns,
                                            uint16_t numCommittedMeasurements) {
    auto result = _bucketCatalog->insert(_opCtx, ns, BSON(_timeField << Date_t::now()));
    auto& [bucketId, commitInfo] = result.getValue();
    ASSERT(!commitInfo);

    _commit(bucketId, numCommittedMeasurements);
}

TEST_F(BucketCatalogTest, InsertIntoSameBucket) {
    // The first insert should be the committer.
    auto result1 = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now()));
    ASSERT(!result1.getValue().commitInfo);

    // A subsequent insert into the same bucket should be a waiter.
    auto result2 = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now()));
    ASSERT(result2.getValue().commitInfo);
    ASSERT(!result2.getValue().commitInfo->isReady());

    // Committing should return both documents since they belong in the same bucket.
    auto data = _bucketCatalog->commit(result1.getValue().bucketId);
    ASSERT_EQ(data.docs.size(), 2);
    ASSERT_EQ(data.numCommittedMeasurements, 0);
    ASSERT(!result2.getValue().commitInfo->isReady());

    // Once the commit has occurred, the waiter should be notified.
    data = _bucketCatalog->commit(result1.getValue().bucketId, _commitInfo);
    ASSERT_EQ(data.docs.size(), 0);
    ASSERT_EQ(data.numCommittedMeasurements, 2);
    ASSERT(result2.getValue().commitInfo->isReady());
}

TEST_F(BucketCatalogTest, GetMetadataReturnsEmptyDocOnMissingBucket) {
    auto bucketId =
        _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now())).getValue().bucketId;
    _bucketCatalog->clear(bucketId);
    ASSERT_BSONOBJ_EQ(BSONObj(), _bucketCatalog->getMetadata(bucketId));
}

TEST_F(BucketCatalogTest, InsertIntoDifferentBuckets) {
    // The first insert should be the committer.
    auto result1 = _bucketCatalog->insert(
        _opCtx, _ns1, BSON(_timeField << Date_t::now() << _metaField << "123"));
    ASSERT(!result1.getValue().commitInfo);

    // Subsequent inserts into different buckets should also be committers.
    auto result2 = _bucketCatalog->insert(
        _opCtx, _ns1, BSON(_timeField << Date_t::now() << _metaField << BSONObj()));
    ASSERT(!result2.getValue().commitInfo);

    auto result3 = _bucketCatalog->insert(_opCtx, _ns2, BSON(_timeField << Date_t::now()));
    ASSERT(!result3.getValue().commitInfo);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(BSON(_metaField << "123"),
                      _bucketCatalog->getMetadata(result1.getValue().bucketId));
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONObj()),
                      _bucketCatalog->getMetadata(result2.getValue().bucketId));
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONNULL),
                      _bucketCatalog->getMetadata(result3.getValue().bucketId));

    // Committing one bucket should only return the one document in that bucket and shoukd not
    // affect the other bucket.
    for (const auto& bucketId :
         {result1.getValue().bucketId, result2.getValue().bucketId, result3.getValue().bucketId}) {
        _commit(bucketId, 0);
    }
}

TEST_F(BucketCatalogTest, NumCommittedMeasurementsAccumulates) {
    // The numCommittedMeasurements returned when committing should accumulate as more entries in
    // the bucket are committed.
    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns1, 1);
}

TEST_F(BucketCatalogTest, ClearNamespaceBuckets) {
    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 0);

    _bucketCatalog->clear(_ns1);

    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 1);
}

TEST_F(BucketCatalogTest, ClearDatabaseBuckets) {
    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 0);
    _insertOneAndCommit(_ns3, 0);

    _bucketCatalog->clear(_ns1.db());

    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 0);
    _insertOneAndCommit(_ns3, 1);
}

DEATH_TEST_F(BucketCatalogTest, CannotProvideCommitInfoOnFirstCommit, "invariant") {
    auto result = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now()));
    auto& [bucketId, _] = result.getValue();
    _bucketCatalog->commit(bucketId, _commitInfo);
}

TEST_F(BucketCatalogWithoutMetadataTest, GetMetadataReturnsEmptyDoc) {
    auto result = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now()));
    ASSERT(!result.getValue().commitInfo);

    ASSERT_BSONOBJ_EQ(BSONObj(), _bucketCatalog->getMetadata(result.getValue().bucketId));

    _commit(result.getValue().bucketId, 0);
}

TEST_F(BucketCatalogWithoutMetadataTest, CommitReturnsNewFields) {
    // Creating a new bucket should return all fields from the initial measurement.
    auto result =
        _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << 0));
    auto& [bucketId, _] = result.getValue();
    auto data = _bucketCatalog->commit(bucketId);
    ASSERT_EQ(2U, data.newFieldNamesToBeInserted.size()) << data.toBSON();
    ASSERT(data.newFieldNamesToBeInserted.count(_timeField)) << data.toBSON();
    ASSERT(data.newFieldNamesToBeInserted.count("a")) << data.toBSON();

    // Inserting a new measurement with the same fields should return an empty set of new fields.

    ASSERT_OK(_bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << 1))
                  .getStatus());
    data = _bucketCatalog->commit(bucketId, _commitInfo);
    ASSERT_EQ(0U, data.newFieldNamesToBeInserted.size()) << data.toBSON();

    // Insert a new measurement with the a new field.
    ASSERT_OK(_bucketCatalog
                  ->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << 2 << "b" << 2))
                  .getStatus());
    data = _bucketCatalog->commit(bucketId, _commitInfo);
    ASSERT_EQ(1U, data.newFieldNamesToBeInserted.size()) << data.toBSON();
    ASSERT(data.newFieldNamesToBeInserted.count("b")) << data.toBSON();

    // Fill up the bucket.
    for (auto i = 3; i < gTimeseriesBucketMaxCount; ++i) {
        ASSERT_OK(
            _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << i))
                .getStatus());
        data = _bucketCatalog->commit(bucketId, _commitInfo);
        ASSERT_EQ(0U, data.newFieldNamesToBeInserted.size()) << i << ":" << data.toBSON();
    }

    // When a bucket overflows, committing to the new overflow bucket should return the fields of
    // the first measurement as new fields.
    auto result2 = _bucketCatalog->insert(
        _opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << gTimeseriesBucketMaxCount));
    auto& [overflowBucketId, unusedCommitInfo] = result2.getValue();
    ASSERT_NE(*bucketId, *overflowBucketId);
    data = _bucketCatalog->commit(overflowBucketId);
    ASSERT_EQ(2U, data.newFieldNamesToBeInserted.size()) << data.toBSON();
    ASSERT(data.newFieldNamesToBeInserted.count(_timeField)) << data.toBSON();
    ASSERT(data.newFieldNamesToBeInserted.count("a")) << data.toBSON();
}
}  // namespace
}  // namespace mongo
