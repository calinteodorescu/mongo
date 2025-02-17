/**
 * Tests that a failing index build on a secondary node causes the primary node to abort the build.
 *
 * @tags: [
 *   featureFlagIndexBuildGracefulErrorHandling,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load('jstests/libs/fail_point_util.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection('test');

// Avoid optimization on empty colls.
assert.commandWorked(coll.insert({a: 1}));

// Pause the index builds on the secondary, using the 'hangAfterStartingIndexBuild' failpoint.
const failpointHangAfterInit = configureFailPoint(secondaryDB, "hangAfterInitializingIndexBuild");

// Create the index and start the build. Set commitQuorum of 2 nodes explicitly, otherwise as only
// primary is voter, it would immediately commit.
const createIdx = IndexBuildTest.startIndexBuild(
    primary, coll.getFullName(), {a: 1}, {}, [ErrorCodes.IndexBuildAborted], /*commitQuorum: */ 2);

failpointHangAfterInit.wait();

// Extract the index build UUID.
const buildUUID =
    IndexBuildTest
        .assertIndexes(secondaryColl, 2, ['_id_'], ['a_1'], {includeBuildUUIDs: true})['a_1']
        .buildUUID;

const failSecondaryBuild =
    configureFailPoint(secondaryDB,
                       "failIndexBuildWithError",
                       {buildUUID: buildUUID, error: ErrorCodes.OutOfDiskSpace});

// Unblock index builds, causing the failIndexBuildWithError failpoint to throw an error.
failpointHangAfterInit.off();

// ErrorCodes.IndexBuildAborted was specified as the expected failure in startIndexBuild.
createIdx();

failSecondaryBuild.off();

// Assert index does not exist.
IndexBuildTest.assertIndexes(coll, 1, ['_id_'], []);
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_'], []);

rst.stopSet();
})();
