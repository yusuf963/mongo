/**
 * If a user attempts to downgrade the server while there is an index build in progress, the
 * downgrade should succeed without blocking.
 * @tags: [
 *   requires_replication,
 *   sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

// TODO SERVER-53953: Remove to re-enable test for feature flag enabled variants.
const res = assert.commandWorked(
    primary.adminCommand({getParameter: 1, featureFlagUseSecondaryDelaySecs: 1}),
    "Failed to call getParameter on feature flag: featureFlagUseSecondaryDelaySecs");
const featureFlagUseSecondaryDelaySecsEnabled = res.featureFlagUseSecondaryDelaySecs.value;

if (featureFlagUseSecondaryDelaySecsEnabled) {
    jsTestLog("Skipping test because the useSecondaryDelaySecs feature flag is enabled");
    rst.stopSet();
    return;
}

assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'a_1');

// Downgrade the primary using the setFeatureCompatibilityVersion command.
try {
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
} finally {
    IndexBuildTest.resumeIndexBuilds(primary);
}

IndexBuildTest.waitForIndexBuildToStop(testDB);

createIdx();

IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

// This confirms that the downgrade command will complete successfully after the index build has
// completed.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

rst.stopSet();
})();
