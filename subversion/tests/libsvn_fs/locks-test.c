/* lock-test.c --- tests for the filesystem locking functions
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

/* TODO how many of these can I get rid of? */
#include <stdlib.h>
#include <string.h>
#include <apr_pools.h>
#include <apr_time.h>
#include <apr_md5.h>

#include "svn_pools.h"
#include "svn_error.h"
#include "svn_time.h"
#include "svn_fs.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_md5.h"
#include "svn_props.h"

#include "../fs-helpers.h"

#include "../../libsvn_fs/fs-loader.h"

#include "../../libsvn_fs_base/fs.h"
#include "../../libsvn_fs_base/dag.h"
#include "../../libsvn_fs_base/node-rev.h"
#include "../../libsvn_fs_base/trail.h"

#include "../../libsvn_fs_base/bdb/rev-table.h"
#include "../../libsvn_fs_base/bdb/txn-table.h"
#include "../../libsvn_fs_base/bdb/nodes-table.h"

#include "../../libsvn_delta/delta.h"

#define SET_STR(ps, s) ((ps)->data = (s), (ps)->len = strlen(s))


/*-----------------------------------------------------------------*/

/** The actual lock-tests called by `make check` **/



/* Test that we can create a lock--nothing more.  */
static svn_error_t *
lock_only (const char **msg,
           svn_boolean_t msg_only,
           svn_test_opts_t *opts,
           apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock;
  
  *msg = "lock only";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-lock-only", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* We are now 'bubba'. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));

  /* Lock /A/D/G/rho. */
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  return SVN_NO_ERROR;
}





/* Test that we can create, fetch, and destroy a lock.  It exercises
   each of the five public fs locking functions.  */
static svn_error_t *
lookup_lock_by_path (const char **msg,
                     svn_boolean_t msg_only,
                     svn_test_opts_t *opts,
                     apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock, *somelock;
  
  *msg = "lookup lock by path";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-lookup-lock-by-path", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* We are now 'bubba'. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));

  /* Lock /A/D/G/rho. */
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  /* Can we look up the lock by path? */
  SVN_ERR (svn_fs_get_lock (&somelock, fs, "/A/D/G/rho", pool));
  if ((! somelock) || (strcmp (somelock->token, mylock->token) != 0))
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Couldn't look up a lock by pathname.");

  return SVN_NO_ERROR;
}

/* Test that we can create a lock outside of the fs and attach it to a
   path.  */
static svn_error_t *
attach_lock (const char **msg,
             svn_boolean_t msg_only,
             svn_test_opts_t *opts,
             apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *somelock;
  svn_lock_t mylock;

  *msg = "attach lock";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-attach-lock", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* We are now 'bubba'. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));

  mylock.path = "/A/D/G/rho";
  mylock.token = "abog-usto-ken";
  mylock.owner = "bubba";
  mylock.comment = "This is a comment.  Yay comment!";
  mylock.creation_date = apr_time_now();
  mylock.expiration_date = apr_time_now() + apr_time_from_sec(3);

  SVN_ERR (svn_fs_attach_lock (&mylock, fs, FALSE, SVN_INVALID_REVNUM, pool));

  /* Can we look up the lock by path? */
  SVN_ERR (svn_fs_get_lock (&somelock, fs, "/A/D/G/rho", pool));
  if ((! somelock) || (strcmp (somelock->token, mylock.token) != 0))
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Couldn't look up a lock by pathname.");

  /* Unlock /A/D/G/rho, and verify that it's gone. */
  SVN_ERR (svn_fs_unlock (fs, mylock.path, mylock.token, 0, pool));
  SVN_ERR (svn_fs_get_lock (&somelock, fs, "/A/D/G/rho", pool));
  if (somelock)
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Removed a lock, but it's still there.");

  return SVN_NO_ERROR;
}


/* This implements the svn_fs_get_locks_callback_t interface, where
   BATON is just a pointer to an integer. */
static svn_error_t *
get_locks_counter_callback (void *baton, 
                            svn_lock_t *lock, 
                            apr_pool_t *pool)
{
  int *lock_count = baton;
  (*lock_count)++;
  return SVN_NO_ERROR;
}

/* Test that we can get all locks under a directory. */
static svn_error_t *
get_locks (const char **msg,
           svn_boolean_t msg_only,
           svn_test_opts_t *opts,
           apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock;
  int num_locks = 0;

  *msg = "get locks";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-get-locks", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* We are now 'bubba'. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));

  /* Lock all paths under /A/D/G. */
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/pi", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/tau", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  SVN_ERR (svn_fs_get_locks (fs, "/A/D/G", get_locks_counter_callback, 
                             &num_locks, pool));
  if (num_locks != 3)
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Failed to retrieve all 3 locks under '/A/D/G'");

  return SVN_NO_ERROR;
}

/* Test that we can create, fetch, and destroy a lock.  It exercises
   each of the five public fs locking functions.  */
static svn_error_t *
basic_lock (const char **msg,
            svn_boolean_t msg_only,
            svn_test_opts_t *opts,
            apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock, *somelock;
  
  *msg = "basic locking";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-basic-lock", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* We are now 'bubba'. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));

  /* Lock /A/D/G/rho. */
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  /* Can we look up the lock by path? */
  SVN_ERR (svn_fs_get_lock (&somelock, fs, "/A/D/G/rho", pool));
  if ((! somelock) || (strcmp (somelock->token, mylock->token) != 0))
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Couldn't look up a lock by pathname.");
    
  /* Unlock /A/D/G/rho, and verify that it's gone. */
  SVN_ERR (svn_fs_unlock (fs, mylock->path, mylock->token, 0, pool));
  SVN_ERR (svn_fs_get_lock (&somelock, fs, "/A/D/G/rho", pool));
  if (somelock)
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Removed a lock, but it's still there.");

  return SVN_NO_ERROR;
}



/* Test that locks are enforced -- specifically that both a username
   and token are required to make use of the lock.  */
static svn_error_t *
lock_credentials (const char **msg,
                  svn_boolean_t msg_only,
                  svn_test_opts_t *opts,
                  apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock;
  svn_error_t *err;

  *msg = "test that locking requires proper credentials";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-lock-credentials", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* We are now 'bubba'. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));

  /* Lock /A/D/G/rho. */
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  /* Push the proper lock-token into the fs access context. */
  SVN_ERR (svn_fs_access_add_lock_token (access, mylock->token));

  /* Make a new transaction and change rho. */
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, newrev, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_test__set_file_contents (txn_root, "/A/D/G/rho",
                                        "new contents", pool));

  /* We are no longer 'bubba'.  We're nobody. */
  SVN_ERR (svn_fs_set_access (fs, NULL));

  /* Try to commit the file change.  Should fail, because we're nobody. */
  err = svn_fs_commit_txn (&conflict, &newrev, txn, pool);
  if (! err)
    return svn_error_create 
      (SVN_ERR_TEST_FAILED, NULL,
       "Uhoh, able to commit locked file without any fs username.");
    
  /* We are now 'hortense'. */
  SVN_ERR (svn_fs_create_access (&access, "hortense", pool));
  SVN_ERR (svn_fs_set_access (fs, access));

  /* Try to commit the file change.  Should fail, because we're 'hortense'. */
  err = svn_fs_commit_txn (&conflict, &newrev, txn, pool);
  if (! err)
    return svn_error_create 
      (SVN_ERR_TEST_FAILED, NULL,
       "Uhoh, able to commit locked file as non-owner.");

  /* Be 'bubba' again. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));
  
  /* Try to commit the file change.  Should fail, because there's no token. */
  err = svn_fs_commit_txn (&conflict, &newrev, txn, pool);
  if (! err)
    return svn_error_create 
      (SVN_ERR_TEST_FAILED, NULL,
       "Uhoh, able to commit locked file with no lock token.");
  
  /* Push the proper lock-token into the fs access context. */
  SVN_ERR (svn_fs_access_add_lock_token (access, mylock->token));

  /* Commit should now succeed. */
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  return SVN_NO_ERROR;
}



/* Test that locks are enforced at commit time.  Somebody might lock
   something behind your back, right before you run
   svn_fs_commit_txn().  Also, this test verifies that recursive
   lock-checks on directories is working properly. */
static svn_error_t *
final_lock_check (const char **msg,
                  svn_boolean_t msg_only,
                  svn_test_opts_t *opts,
                  apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock;
  svn_error_t *err;

  *msg = "test that locking is enforced in final commit step";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-final-lock-check", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* Make a new transaction and delete "/A" */
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, newrev, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_delete (txn_root, "/A", pool));

  /* Become 'bubba' and lock "/A/D/G/rho". */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  /* We are no longer 'bubba'.  We're nobody. */
  SVN_ERR (svn_fs_set_access (fs, NULL));

  /* Try to commit the transaction.  Should fail, because a child of
     the deleted directory is locked by someone else. */
  err = svn_fs_commit_txn (&conflict, &newrev, txn, pool);
  if (! err)
    return svn_error_create 
      (SVN_ERR_TEST_FAILED, NULL,
       "Uhoh, able to commit dir deletion when a child is locked.");

  /* Supply correct username and token;  commit should work. */
  SVN_ERR (svn_fs_set_access (fs, access));
  SVN_ERR (svn_fs_access_add_lock_token (access, mylock->token));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  return SVN_NO_ERROR;
}



/* If a directory's child is locked by someone else, we should still
   be able to commit a propchange on the directory. */
static svn_error_t *
lock_dir_propchange (const char **msg,
                     svn_boolean_t msg_only,
                     svn_test_opts_t *opts,
                     apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock;

  *msg = "dir propchange can be committed with locked child";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-lock-dir-propchange", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* Become 'bubba' and lock "/A/D/G/rho". */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  /* We are no longer 'bubba'.  We're nobody. */
  SVN_ERR (svn_fs_set_access (fs, NULL));

  /* Make a new transaction and make a propchange on "/A" */
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, newrev, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_change_node_prop (txn_root, "/A",
                                    "foo", svn_string_create ("bar", pool),
                                    pool));

  /* Commit should succeed;  this means we're doing a non-recursive
     lock-check on directory, rather than a recursive one.  */
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  return SVN_NO_ERROR;
}



/* DAV clients sometimes LOCK non-existent paths, as a way of
   reserving names.  Check that this technique works. */
static svn_error_t *
lock_name_reservation (const char **msg,
                       svn_boolean_t msg_only,
                       svn_test_opts_t *opts,
                       apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root, *rev_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock;
  svn_error_t *err;

  *msg = "able to reserve a name (lock non-existent path)";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-lock-name-reservation", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* Become 'bubba' and lock imaginary path  "/A/D/G2/blooga". */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G2/blooga", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  /* We are no longer 'bubba'.  We're nobody. */
  SVN_ERR (svn_fs_set_access (fs, NULL));

  /* Make a new transaction. */
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, newrev, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* This copy should fail, because an imaginary path in the target of
     the copy is reserved by someone else. */
  SVN_ERR (svn_fs_revision_root (&rev_root, fs, 1, pool));
  err = svn_fs_copy (rev_root, "/A/D/G", txn_root, "/A/D/G2", pool);
  if (! err)
    return svn_error_create 
      (SVN_ERR_TEST_FAILED, NULL,
       "Uhoh, copy succeeded when path within target was locked.");
   
  return SVN_NO_ERROR;
}



/* Test that locks auto-expire correctly. */
static svn_error_t *
lock_expiration (const char **msg,
                 svn_boolean_t msg_only,
                 svn_test_opts_t *opts,
                 apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock;
  svn_error_t *err;

  *msg = "test that locks can expire";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-lock-expiration", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* Make a new transaction and change rho. */
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, newrev, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_test__set_file_contents (txn_root, "/A/D/G/rho",
                                        "new contents", pool));

  /* We are now 'bubba'. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));

  /* Lock /A/D/G/rho, with an expiration 3 seconds from now. */
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 3,
                        SVN_INVALID_REVNUM, pool));

  /* Become nobody. */
  SVN_ERR (svn_fs_set_access (fs, NULL));
  
  /* Try to commit.  Should fail because we're 'nobody', and the lock
     hasn't expired yet. */
  err = svn_fs_commit_txn (&conflict, &newrev, txn, pool);
  if (! err)
    return svn_error_create 
      (SVN_ERR_TEST_FAILED, NULL,
       "Uhoh, able to commit a file that has a non-expired lock.");

  /* Sleep 5 seconds, so the lock auto-expires.  Anonymous commit
     should then succeed. */
  apr_sleep (apr_time_from_sec(5));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  return SVN_NO_ERROR;
}

/* Test that a lock can be broken, stolen, or refreshed */
static svn_error_t *
lock_break_steal_refresh (const char **msg,
                          svn_boolean_t msg_only,
                          svn_test_opts_t *opts,
                          apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock, *somelock;

  *msg = "breaking, stealing, refreshing a lock";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-steal-refresh", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* Become 'bubba' and lock "/A/D/G/rho". */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));

  /* Become 'hortense' and break bubba's lock, then verify it's gone. */
  SVN_ERR (svn_fs_create_access (&access, "hortense", pool));
  SVN_ERR (svn_fs_set_access (fs, access));
  SVN_ERR (svn_fs_unlock (fs, mylock->path, mylock->token,
                          1 /* FORCE BREAK */, pool));
  SVN_ERR (svn_fs_get_lock (&somelock, fs, "/A/D/G/rho", pool));
  if (somelock)
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Tried to break a lock, but it's still there.");

  /* As hortense, create a new lock, and verify that we own it. */
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0,
                        SVN_INVALID_REVNUM, pool));
  SVN_ERR (svn_fs_get_lock (&somelock, fs, "/A/D/G/rho", pool));
  if (strcmp (somelock->owner, mylock->owner) != 0)
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Made a lock, but we don't seem to own it.");
  
  /* As bubba, steal hortense's lock, creating a new one that expires. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "",
                        1 /* FORCE STEAL */,
                        300 /* expire in 5 minutes */,
                        SVN_INVALID_REVNUM, pool));
  SVN_ERR (svn_fs_get_lock (&somelock, fs, "/A/D/G/rho", pool));
  if (strcmp (somelock->owner, mylock->owner) != 0)
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Made a lock, but we don't seem to own it.");
  if (! somelock->expiration_date)
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Made expiring lock, but seems not to expire.");
    
  /* Refresh the lock, so that it never expires. */
  somelock->expiration_date = 0;
  SVN_ERR (svn_fs_attach_lock (somelock, fs, TRUE, SVN_INVALID_REVNUM, pool));
  SVN_ERR (svn_fs_get_lock (&somelock, fs, "/A/D/G/rho", pool));
  if (somelock->expiration_date)
    return svn_error_create (SVN_ERR_TEST_FAILED, NULL,
                             "Made non-expirirng lock, but it expires.");  

  return SVN_NO_ERROR;
}


/* Test that svn_fs_lock() and svn_fs_attach_lock() can do
   out-of-dateness checks..  */
static svn_error_t *
lock_out_of_date (const char **msg,
                  svn_boolean_t msg_only,
                  svn_test_opts_t *opts,
                  apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t newrev;
  svn_fs_access_t *access;
  svn_lock_t *mylock;
  svn_error_t *err;
  
  *msg = "check out-of-dateness before locking";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Prepare a filesystem and a new txn. */
  SVN_ERR (svn_test__create_any_fs (&fs, "test-repo-lock-out-of-date", 
                                    opts->fs_type, pool));
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, 0, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree and commit it. */
  SVN_ERR (svn_test__create_greek_tree (txn_root, pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));

  /* Commit a small change to /A/D/G/rho, creating revision 2. */
  SVN_ERR (svn_fs_begin_txn2 (&txn, fs, newrev, SVN_FS_TXN_CHECK_LOCKS, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_test__set_file_contents (txn_root, "/A/D/G/rho",
                                        "new contents", pool));
  SVN_ERR (svn_fs_commit_txn (&conflict, &newrev, txn, pool));
  
  /* We are now 'bubba'. */
  SVN_ERR (svn_fs_create_access (&access, "bubba", pool));
  SVN_ERR (svn_fs_set_access (fs, access));

  /* Try to lock /A/D/G/rho, but claim that we still have r1 of the file. */
  err = svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0, 1, pool);
  if (! err)
    return svn_error_create 
      (SVN_ERR_TEST_FAILED, NULL,
       "Uhoh, able to lock an out-of-date file.");

  /* Attempt lock again, this time claiming to have r2. */
  SVN_ERR (svn_fs_lock (&mylock, fs, "/A/D/G/rho", "", 0, 0, 2, pool));

  /* 'Refresh' the lock, claiming to have r1... should fail. */
  mylock->expiration_date = apr_time_now() + (50 * APR_USEC_PER_SEC);
  err = svn_fs_attach_lock (mylock, fs, TRUE, 1, pool);
  if (! err)
    return svn_error_create 
      (SVN_ERR_TEST_FAILED, NULL,
       "Uhoh, able to refresh a lock on an out-of-date file.");

  return SVN_NO_ERROR;
}



/* ------------------------------------------------------------------------ */

/* The test table.  */

struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS (lock_only),
    SVN_TEST_PASS (lookup_lock_by_path),
    SVN_TEST_PASS (attach_lock),
    SVN_TEST_PASS (get_locks),
    SVN_TEST_PASS (basic_lock),
    SVN_TEST_PASS (lock_credentials),
    SVN_TEST_PASS (final_lock_check),
    SVN_TEST_PASS (lock_dir_propchange),
    SVN_TEST_PASS (lock_name_reservation),
    SVN_TEST_PASS (lock_expiration),
    SVN_TEST_PASS (lock_break_steal_refresh),
    SVN_TEST_PASS (lock_out_of_date),
    SVN_TEST_NULL
  };


