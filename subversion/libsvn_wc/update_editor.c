/*
 * update_editor.c :  main editor for checkouts and updates
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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



#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_md5.h>
#include <apr_tables.h>
#include <apr_file_io.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_delta.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "svn_error.h"
#include "svn_io.h"
#include "svn_md5.h"
#include "svn_base64.h"
#include "svn_hash.h"
#include "svn_wc.h"
#include "svn_private_config.h"
#include "svn_time.h"

#include "wc.h"
#include "log.h"
#include "adm_files.h"
#include "adm_ops.h"
#include "entries.h"
#include "props.h"


/*** batons ***/

struct edit_baton
{
  /* For updates, the "destination" of the edit is the ANCHOR (the
     directory at which the edit is rooted) plus the TARGET (the
     actual thing we wish to update).  For checkouts, ANCHOR holds the
     whole path, and TARGET is unused. */
  const char *anchor;
  const char *target;

  /* ADM_ACCESS is an access baton that includes the ANCHOR directory */
  svn_wc_adm_access_t *adm_access;

  /* The revision we're targeting...or something like that. */
  svn_revnum_t target_revision;

  /* Whether this edit will descend into subdirs */
  svn_boolean_t recurse;

  /* Need to know if the user wants us to overwrite the 'now' times on
     edited/added files with the last-commit-time. */
  svn_boolean_t use_commit_times;

  /* Was the root actually opened (was this a non-empty edit)? */
  svn_boolean_t root_opened;

  /* Was the update-target deleted?  This is a special situation. */
  svn_boolean_t target_deleted;
 
  /* Non-null if this is a 'switch' operation. */
  const char *switch_url;

  /* External diff3 to use for merges (can be null, in which case
     internal merge code is used). */
  const char *diff3_cmd;

  /* Object for gathering info to be accessed after the edit is
     complete. */
  svn_wc_traversal_info_t *traversal_info;

  /* This editor sends back notifications as it edits. */
  svn_wc_notify_func_t notify_func;
  void *notify_baton;

  /* This editor is normally wrapped in a cancellation editor anyway,
     so it doesn't bother to check for cancellation itself.  However,
     it needs a cancel_func and cancel_baton available to pass to
     long-running functions. */
  svn_cancel_func_t cancel_func;
  void *cancel_baton;

  apr_pool_t *pool;
};


struct dir_baton
{
  /* The path to this directory. */
  const char *path;

  /* Basename of this directory. */
  const char *name;

  /* The repository URL this directory will correspond to. */
  const char *new_URL;

  /* The global edit baton. */
  struct edit_baton *edit_baton;

  /* Baton for this directory's parent, or NULL if this is the root
     directory. */
  struct dir_baton *parent_baton;

  /* Gets set iff this is a new directory that is not yet versioned and not
     yet in the parent's list of entries */
  svn_boolean_t added;

  /* An array of svn_prop_t structures, representing all the property
     changes to be applied to this directory. */
  apr_array_header_t *propchanges;

  /* The bump information for this directory. */
  struct bump_dir_info *bump_info;

  /* The pool in which this baton itself is allocated. */
  apr_pool_t *pool;
};


/* The bump information is tracked separately from the directory batons.
   This is a small structure kept in the edit pool, while the heavier
   directory baton is managed by the editor driver.

   In a postfix delta case, the directory batons are going to disappear.
   The files will refer to these structures, rather than the full
   directory baton.  */
struct bump_dir_info
{
  /* ptr to the bump information for the parent directory */
  struct bump_dir_info *parent;

  /* how many entries are referring to this bump information? */
  int ref_count;

  /* the path of the directory to bump */
  const char *path;
};


struct handler_baton
{
  apr_file_t *source;
  apr_file_t *dest;
  svn_txdelta_window_handler_t apply_handler;
  void *apply_baton;
  apr_pool_t *pool;
  struct file_baton *fb;
};


/* Return the url for NAME in DIR, allocated in POOL, or null if
 * unable to obtain a url.  If NAME is null, get the url for DIR.
 * 
 * Use ASSOCIATED_ACCESS to retrieve an access baton for PATH, and do
 * all temporary allocation in POOL. 
 */
static const char *
get_entry_url (svn_wc_adm_access_t *associated_access,
               const char *dir,
               const char *name,
               apr_pool_t *pool)
{
  svn_error_t *err;
  const svn_wc_entry_t *entry;
  svn_wc_adm_access_t *adm_access;

  err = svn_wc_adm_retrieve (&adm_access, associated_access, dir, pool);

  if (! err)
    {
      /* Note that `name' itself may be NULL. */
      err = svn_wc_entry (&entry, svn_path_join_many (pool, dir, name, NULL),
                          adm_access, FALSE, pool);
    }
  if (err || (! entry) || (! entry->url))
    {
      if (err)
        svn_error_clear (err);
      return NULL;
    }

  return entry->url;
}


/* Return a new dir_baton to represent NAME (a subdirectory of
   PARENT_BATON).  If PATH is NULL, this is the root directory of the
   edit. */
static struct dir_baton *
make_dir_baton (const char *path,
                struct edit_baton *eb,
                struct dir_baton *pb,
                svn_boolean_t added,
                apr_pool_t *pool)
{
  struct dir_baton *d = apr_pcalloc (pool, sizeof (*d));
  struct bump_dir_info *bdi;
  
  /* Don't do this.  Just do NOT do this to me. */
  if (pb && (! path))
    abort();

  /* Construct the PATH and baseNAME of this directory. */
  d->path = apr_pstrdup (pool, eb->anchor);
  if (path)
    {
      d->path = svn_path_join (d->path, path, pool);
      d->name = svn_path_basename (path, pool);
    }
  else
    {
      d->name = NULL;
    }

  /* Figure out the new_URL for this directory. */
  if (eb->switch_url)
    {
      /* Switches are, shall we say, complex.  If this directory is
         the root directory (it has no parent), then it either gets
         the SWITCH_URL for its own (if it is both anchor and target)
         or the parent of the SWITCH_URL (if it is anchor, but there's
         another target). */
      if (! pb)
        {
          if (eb->target) /* the anchor is also the target */
            d->new_URL = apr_pstrdup (pool, eb->switch_url);
          else
            d->new_URL = svn_path_dirname (eb->switch_url, pool);
        }
      /* If this directory is *not* the root (has a parent), but it is
         the target (has no grandparent), then it gets the SWITCH_URL
         for its own.  Otherwise, it gets a child of its parent's
         URL. */
      else
        {
          if (! pb->parent_baton)
            d->new_URL = apr_pstrdup (pool, eb->switch_url);
          else
            d->new_URL = svn_path_url_add_component (pb->new_URL, 
                                                     d->name, pool);
        }
    }
  else  /* must be an update */
    {
      /* updates are the odds ones.  if we're updating a path already
         present on disk, we use its original URL.  otherwise, we'll
         telescope based on its parent's URL. */
      d->new_URL = get_entry_url (eb->adm_access, d->path, NULL, pool);
      if ((! d->new_URL) && pb)
        d->new_URL = svn_path_url_add_component (pb->new_URL, d->name, pool);
    }

  /* the bump information lives in the edit pool */
  bdi = apr_palloc (eb->pool, sizeof (*bdi));
  bdi->parent = pb ? pb->bump_info : NULL;
  bdi->ref_count = 1;
  bdi->path = apr_pstrdup (eb->pool, d->path);

  /* the parent's bump info has one more referer */
  if (pb)
    ++bdi->parent->ref_count;

  d->edit_baton   = eb;
  d->parent_baton = pb;
  d->pool         = pool;
  d->propchanges  = apr_array_make (pool, 1, sizeof (svn_prop_t));
  d->added        = added;
  d->bump_info    = bdi;

  return d;
}



/* Helper for maybe_bump_dir_info():

   In a single atomic action, (1) remove any 'deleted' entries from a
   directory, (2) remove any 'missing' dir entries, and (3) remove the
   directory's 'incomplete' flag. */
static svn_error_t *
complete_directory (struct edit_baton *eb,
                    const char *path,
                    svn_boolean_t is_root_dir,
                    apr_pool_t *pool)
{
  svn_wc_adm_access_t *adm_access;
  apr_hash_t *entries;
  svn_wc_entry_t *entry;
  apr_hash_index_t *hi;
  apr_pool_t *subpool;
  svn_wc_entry_t *current_entry;
  const char *name;

  /* All operations are on the in-memory entries hash. */
  SVN_ERR (svn_wc_adm_retrieve (&adm_access, eb->adm_access, path, pool));
  SVN_ERR (svn_wc_entries_read (&entries, adm_access, TRUE, pool));

  /* Mark THIS_DIR complete. */
  entry = apr_hash_get (entries, SVN_WC_ENTRY_THIS_DIR, APR_HASH_KEY_STRING);
  if (! entry)
    return svn_error_createf (SVN_ERR_ENTRY_NOT_FOUND, NULL,
                              "No '.' entry in: '%s'", path);
  entry->incomplete = FALSE;

  /* If this is the root directory, and there was a target, we should
     only be modifying that target!  */
  if (is_root_dir && eb->target)
    {
      name = eb->target;
      current_entry = apr_hash_get (entries, name, APR_HASH_KEY_STRING);
      if (! current_entry)
        goto complete;

      if (current_entry->deleted)
        {
          /* If the target of the update is 'deleted', we leave it be.
             see r6748, issue #919.
             
             For those confused: name_deleted is a global state; it
             turns out that if we're here, the deleted entry we're
             seeing *was* the target of the update.  close_dir() is
             being called on the anchor directory (originally opened
             by open_root()), and thus we're looking here at the
             deleted 'target' of the update. */
          if (! eb->target_deleted)
            svn_wc__entry_remove (entries, name);
        }
      else if (current_entry->kind == svn_node_dir)
        {
          const char *child_path = svn_path_join (path, name, pool);
          
          if ((svn_wc__adm_missing (adm_access, child_path))
              && (current_entry->schedule != svn_wc_schedule_add))
            {
              svn_wc__entry_remove (entries, name);
              if (eb->notify_func)
                (* eb->notify_func) (eb->notify_baton, child_path, 
                                     svn_wc_notify_update_delete,
                                     current_entry->kind, NULL, 
                                     svn_wc_notify_state_unknown,
                                     svn_wc_notify_state_unknown,
                                     SVN_INVALID_REVNUM);
            }
        }

      goto complete;
    }

  /* Remove any deleted or missing entries. */
  subpool = svn_pool_create (pool);
  for (hi = apr_hash_first (pool, entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;

      svn_pool_clear (subpool);
      apr_hash_this (hi, &key, NULL, &val);
      name = key;
      current_entry = val;
      
      if (current_entry->deleted)
        {
          svn_wc__entry_remove (entries, name);
        }
      else if (current_entry->kind == svn_node_dir)
        {
          const char *child_path = svn_path_join (path, name, subpool);
          
          if ((svn_wc__adm_missing (adm_access, child_path))
              && (current_entry->schedule != svn_wc_schedule_add))
            {
              svn_wc__entry_remove (entries, name);
              if (eb->notify_func)
                (* eb->notify_func) (eb->notify_baton, child_path, 
                                     svn_wc_notify_update_delete,
                                     current_entry->kind, NULL, 
                                     svn_wc_notify_state_unknown,
                                     svn_wc_notify_state_unknown,
                                     SVN_INVALID_REVNUM);
            }
        }
    }

 complete:

  /* An atomic write of the whole entries file. */
  SVN_ERR (svn_wc__entries_write (entries, adm_access, pool));

  return SVN_NO_ERROR;
}



/* Decrement the bump_dir_info's reference count. If it hits zero,
   then this directory is "done". This means it is safe to remove the
   'incomplete' flag attached to the THIS_DIR entry.

   In addition, when the directory is "done", we loop onto the parent's
   bump information to possibly mark it as done, too.
*/
static svn_error_t *
maybe_bump_dir_info (struct edit_baton *eb,
                     struct bump_dir_info *bdi,
                     apr_pool_t *pool)
{
  /* Keep moving up the tree of directories until we run out of parents,
     or a directory is not yet "done".  */
  for ( ; bdi != NULL; bdi = bdi->parent)
    {
      if (--bdi->ref_count > 0)
        return SVN_NO_ERROR;    /* directory isn't done yet */

      /* The refcount is zero, so we remove any 'dead' entries from
         the directory and mark it 'complete'.  */
      SVN_ERR (complete_directory (eb, bdi->path, 
                                   bdi->parent ? FALSE : TRUE, pool));
    }
  /* we exited the for loop because there are no more parents */

  return SVN_NO_ERROR;
}



struct file_baton
{
  /* The global edit baton. */
  struct edit_baton *edit_baton;

  /* Pool specific to this file_baton. */
  apr_pool_t *pool;

  /* Name of this file (its entry in the directory). */
  const char *name;

  /* Path to this file, either abs or relative to the change-root. */
  const char *path;

  /* The repository URL this file will correspond to. */
  const char *new_URL;

  /* Set if this file is new. */
  svn_boolean_t added;

  /* This gets set if the file underwent a text change, which guides
     the code that syncs up the adm dir and working copy. */
  svn_boolean_t text_changed;

  /* This gets set if the file underwent a prop change, which guides
     the code that syncs up the adm dir and working copy. */
  svn_boolean_t prop_changed;

  /* An array of svn_prop_t structures, representing all the property
     changes to be applied to this file. */
  apr_array_header_t *propchanges;

  /* The last-changed-date of the file.  This is actually a property
     that comes through as an 'entry prop', and will be used to set
     the working file's timestamp if it's added.  */
  const char *last_changed_date;

  /* Bump information for the directory this file lives in */
  struct bump_dir_info *bump_info;

  /* This is initialized to all zeroes when the baton is created, then
     populated with the MD5 digest of the resultant fulltext after the
     last window is handled by the handler returned from
     apply_textdelta(). */ 
  unsigned char digest[APR_MD5_DIGESTSIZE];
};


/* Make a new file baton in the provided POOL, with PB as the parent baton.
   PATH is relative to the root of the edit. */
static struct file_baton *
make_file_baton (struct dir_baton *pb,
                 const char *path,
                 svn_boolean_t adding,
                 apr_pool_t *pool)
{
  struct file_baton *f = apr_pcalloc (pool, sizeof (*f));

  /* I rather need this information, yes. */
  if (! path)
    abort();

  /* Make the file's on-disk name. */
  f->path = svn_path_join (pb->edit_baton->anchor, path, pool);
  f->name = svn_path_basename (path, pool);

  /* Figure out the new_URL for this file. */
  if (pb->edit_baton->switch_url)
    {
      f->new_URL = svn_path_url_add_component (pb->new_URL, f->name, pool);
    }
  else 
    {
      f->new_URL = get_entry_url (pb->edit_baton->adm_access,
                                  pb->path, f->name, pool);
    }

  f->pool         = pool;
  f->edit_baton   = pb->edit_baton;
  f->propchanges  = apr_array_make (pool, 1, sizeof (svn_prop_t));
  f->bump_info    = pb->bump_info;
  f->added        = adding;

  /* No need to initialize f->digest, since we used pcalloc(). */

  /* the directory's bump info has one more referer now */
  ++f->bump_info->ref_count;

  return f;
}



/*** Helpers for the editor callbacks. ***/

static svn_error_t *
window_handler (svn_txdelta_window_t *window, void *baton)
{
  struct handler_baton *hb = baton;
  struct file_baton *fb = hb->fb;
  svn_error_t *err = SVN_NO_ERROR, *err2 = SVN_NO_ERROR;

  /* Apply this window.  We may be done at that point.  */
  err = hb->apply_handler (window, hb->apply_baton);
  if (window != NULL && err == SVN_NO_ERROR)
    return err;

  /* Either we're done (window is NULL) or we had an error.  In either
     case, clean up the handler.  */
  if (hb->source)
    {
      err2 = svn_wc__close_text_base (hb->source, fb->path, 0, fb->pool);
      if (err2 != SVN_NO_ERROR && err == SVN_NO_ERROR)
        err = err2;
    }
  err2 = svn_wc__close_text_base (hb->dest, fb->path, 0, fb->pool);
  if (err2 != SVN_NO_ERROR)
    {
      if (err == SVN_NO_ERROR)
        err = err2;
      else
        svn_error_clear (err2);
    }
  svn_pool_destroy (hb->pool);

  if (err != SVN_NO_ERROR)
    {
      /* We failed to apply the patch; clean up the temporary file.  */
      const char *tmppath = svn_wc__text_base_path (fb->path, TRUE, fb->pool);
      apr_file_remove (tmppath, fb->pool);
    }
  else
    {
      /* Leave a note in the baton indicating that there's new text to
         sync up.  */
      fb->text_changed = 1;
    }

  return err;
}


/* Prepare directory for dir_baton DB for updating or checking out.
 *
 * If the path already exists, but is not a working copy for
 * ANCESTOR_URL and ANCESTOR_REVISION, then an error will be returned. 
 */
static svn_error_t *
prep_directory (struct dir_baton *db,
                const char *ancestor_url,
                svn_revnum_t ancestor_revision,
                apr_pool_t *pool)
{
  /* Make sure the directory exists. */
  SVN_ERR (svn_wc__ensure_directory (db->path, pool));

  /* Make sure it's the right working copy, either by creating it so,
     or by checking that it is so already. */
  SVN_ERR (svn_wc_ensure_adm (db->path, NULL,
                              ancestor_url, ancestor_revision, pool));

  if (! db->edit_baton->adm_access
      || strcmp (svn_wc_adm_access_path (db->edit_baton->adm_access),
                 db->path))
    {
      svn_wc_adm_access_t *adm_access;
      apr_pool_t *adm_access_pool
        = db->edit_baton->adm_access
        ? svn_wc_adm_access_pool (db->edit_baton->adm_access)
        : db->edit_baton->pool;

      SVN_ERR (svn_wc_adm_open (&adm_access, db->edit_baton->adm_access,
                                db->path, TRUE, FALSE, adm_access_pool));
      if (!db->edit_baton->adm_access)
        db->edit_baton->adm_access = adm_access;
    }
  
  return SVN_NO_ERROR;
}


/* Accumulate tags in LOG_ACCUM to set ENTRY_PROPS for BASE_NAME.
   ENTRY_PROPS is an array of svn_prop_t* entry props. */
static void
accumulate_entry_props (svn_stringbuf_t *log_accum,
                        const char *base_name,
                        apr_array_header_t *entry_props,
                        apr_pool_t *pool)
{
  int i;

  for (i = 0; i < entry_props->nelts; ++i)
    {
      const svn_prop_t *prop = &APR_ARRAY_IDX (entry_props, i, svn_prop_t);
      const char *entry_field = NULL;

      /* A prop value of NULL means the information was not
         available.  We don't remove this field from the entries
         file; we have convention just leave it empty.  So let's
         just skip those entry props that have no values. */
      if (! prop->value)
        continue;

      if (! strcmp (prop->name, SVN_PROP_ENTRY_LAST_AUTHOR))
        entry_field = SVN_WC__ENTRY_ATTR_CMT_AUTHOR;
      else if (! strcmp (prop->name, SVN_PROP_ENTRY_COMMITTED_REV))
        entry_field = SVN_WC__ENTRY_ATTR_CMT_REV;
      else if (! strcmp (prop->name, SVN_PROP_ENTRY_COMMITTED_DATE))
        entry_field = SVN_WC__ENTRY_ATTR_CMT_DATE;
      else if (! strcmp (prop->name, SVN_PROP_ENTRY_UUID))
        entry_field = SVN_WC__ENTRY_ATTR_UUID;
      else
        continue;

      svn_xml_make_open_tag (&log_accum, pool, svn_xml_self_closing,
                             SVN_WC__LOG_MODIFY_ENTRY,
                             SVN_WC__LOG_ATTR_NAME, base_name,
                             entry_field, prop->value->data,
                             NULL);         
    }
}


/* Accumulate tags in LOG_ACCUM to set WCPROPS for BASE_NAME.  WCPROPS is
   an array of svn_prop_t* wc props. */
static void
accumulate_wcprops (svn_stringbuf_t *log_accum,
                    const char *base_name,
                    apr_array_header_t *wcprops,
                    apr_pool_t *pool)
{
  int i;

  /* ### The log file will rewrite the props file for each property :( It
     ### would be better if all the changes could be combined into one
     ### write. */
  for (i = 0; i < wcprops->nelts; ++i)
    {
      const svn_prop_t *prop = &APR_ARRAY_IDX (wcprops, i, svn_prop_t);

      svn_xml_make_open_tag (&log_accum, pool, svn_xml_self_closing,
                             SVN_WC__LOG_MODIFY_WCPROP,
                             SVN_WC__LOG_ATTR_NAME, base_name,
                             SVN_WC__LOG_ATTR_PROPNAME, prop->name,
                             prop->value ? SVN_WC__LOG_ATTR_PROPVAL : NULL,
                             prop->value ? prop->value->data : NULL,
                             NULL);
    }
}
      

/*** The callbacks we'll plug into an svn_delta_editor_t structure. ***/

static svn_error_t *
set_target_revision (void *edit_baton, 
                     svn_revnum_t target_revision,
                     apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;

  /* Stashing a target_revision in the baton */
  eb->target_revision = target_revision;
  return SVN_NO_ERROR;
}


static svn_error_t *
open_root (void *edit_baton,
           svn_revnum_t base_revision, /* This is ignored in co */
           apr_pool_t *pool,
           void **dir_baton)
{
  struct edit_baton *eb = edit_baton;
  struct dir_baton *d;

  /* Note that something interesting is actually happening in this
     edit run. */
  eb->root_opened = TRUE;

  *dir_baton = d = make_dir_baton (NULL, eb, NULL, FALSE, pool);
  if (! eb->target)
    {
      /* For an update with a NULL target, this is equivalent to open_dir(): */
      svn_wc_adm_access_t *adm_access;
      svn_wc_entry_t tmp_entry;

      /* Mark directory as being at target_revision, but incomplete. */  
      tmp_entry.revision = eb->target_revision;
      tmp_entry.url = d->new_URL;
      tmp_entry.incomplete = TRUE;
      SVN_ERR (svn_wc_adm_retrieve (&adm_access, eb->adm_access,
                                    d->path, pool));
      SVN_ERR (svn_wc__entry_modify (adm_access, NULL /* THIS_DIR */,
                                     &tmp_entry,
                                     SVN_WC__ENTRY_MODIFY_REVISION |
                                     SVN_WC__ENTRY_MODIFY_URL |
                                     SVN_WC__ENTRY_MODIFY_INCOMPLETE,
                                     TRUE /* immediate write */,
                                     pool));
    }

  return SVN_NO_ERROR;
}


/* Helper for delete_entry().

   Search an error chain (ERR) for evidence that a local mod was left.
   If so, cleanup LOGFILE and return an appropriate error.  Otherwise,
   just return the original error chain.
*/
static svn_error_t *
leftmod_error_chain (svn_error_t *err,
                     const char *logfile,
                     const char *path,
                     apr_pool_t *pool)
{
  svn_error_t *tmp_err = SVN_NO_ERROR;

  if (! err)
    return SVN_NO_ERROR;

  /* Advance TMP_ERR to the part of the error chain that reveals that
     a local mod was left, or to the NULL end of the chain. */
  for (tmp_err = err; tmp_err; tmp_err = tmp_err->child)
    if (tmp_err->apr_err == SVN_ERR_WC_LEFT_LOCAL_MOD)
      {
        break;
      }
                   
  /* If we found a "left a local mod" error, wrap and return it.
     Otherwise, we just return our top-most error. */
  if (tmp_err)
    {
      /* Remove the LOGFILE (and eat up errors from this process). */
      svn_error_t *err2 = svn_io_remove_file (logfile, pool);
      if (err2)
        svn_error_clear (err2);

      return svn_error_createf
        (SVN_ERR_WC_OBSTRUCTED_UPDATE, tmp_err,
         "failed to delete dir '%s': local mods found within.", path);
    }

  return err;
}


static svn_error_t *
do_entry_deletion (struct edit_baton *eb,
                   const char *parent_path,
                   const char *path, 
                   apr_pool_t *pool)
{
  apr_status_t apr_err;
  apr_file_t *log_fp = NULL;
  const char *base_name = svn_path_basename (path, pool);
  const char *tgt_rev_str = NULL;
  svn_wc_adm_access_t *adm_access;
  svn_node_kind_t kind;
  const char *logfile_path;
  const char *full_path = svn_path_join (parent_path, base_name, pool);
  svn_stringbuf_t *log_item = svn_stringbuf_create ("", pool);

  SVN_ERR (svn_io_check_path (full_path, &kind, pool));

  SVN_ERR (svn_wc_adm_retrieve (&adm_access, eb->adm_access,
                                parent_path, pool));

  logfile_path = svn_wc__adm_path (parent_path, FALSE, pool,
                                   SVN_WC__ADM_LOG, NULL);

  /* If trying to delete a locally-modified file, throw an 'obstructed
     update' error. */
  if (kind == svn_node_file)
    {
      svn_boolean_t tmodified_p, pmodified_p;
      SVN_ERR (svn_wc_text_modified_p (&tmodified_p, full_path, FALSE,
                                       adm_access, pool));
      SVN_ERR (svn_wc_props_modified_p (&pmodified_p, full_path,
                                        adm_access, pool));

      if (tmodified_p || pmodified_p)
        return svn_error_createf
          (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
           "failed to delete file '%s': file has local modifications.",
           base_name);
    }

  SVN_ERR (svn_wc__open_adm_file (&log_fp,
                                  parent_path,
                                  SVN_WC__ADM_LOG,
                                  (APR_WRITE | APR_CREATE), /* not excl */
                                  pool));

  /* Here's the deal: in the new editor interface, PATH is a full path
     below the editor's anchor, and parent_path is the parent directory.
     That's all fine and well, but our log-system requires that all
     log commands talk *only* about paths relative (and below)
     parent_path, i.e. where the log is being executed.  */

  svn_xml_make_open_tag (&log_item,
                         pool,
                         svn_xml_self_closing,
                         SVN_WC__LOG_DELETE_ENTRY,
                         SVN_WC__LOG_ATTR_NAME,
                         base_name,
                         NULL);

  /* If the thing being deleted is the *target* of this update, then
     we need to recreate a 'deleted' entry, so that parent can give
     accurate reports about itself in the future. */
  if (eb->target
      && (strcmp (path, eb->target) == 0))
    {
      tgt_rev_str = apr_psprintf (pool, "%" SVN_REVNUM_T_FMT,
                                  eb->target_revision);

      svn_xml_make_open_tag (&log_item, pool, svn_xml_self_closing,
                             SVN_WC__LOG_MODIFY_ENTRY,
                             SVN_WC__LOG_ATTR_NAME,
                             path,
                             SVN_WC__ENTRY_ATTR_KIND,
                             (kind == svn_node_file) ? 
                                SVN_WC__ENTRIES_ATTR_FILE_STR :
                                SVN_WC__ENTRIES_ATTR_DIR_STR,
                             SVN_WC__ENTRY_ATTR_REVISION,
                             tgt_rev_str,
                             SVN_WC__ENTRY_ATTR_DELETED,
                             "true",
                             NULL);

      eb->target_deleted = TRUE;
    }

  apr_err = apr_file_write_full (log_fp, log_item->data, log_item->len, NULL);
  if (apr_err)
    {
      apr_file_close (log_fp);
      return svn_error_createf (apr_err, NULL,
                                "delete error writing log file for '%s'.",
                                parent_path);
    }

  SVN_ERR (svn_wc__close_adm_file (log_fp,
                                   parent_path,
                                   SVN_WC__ADM_LOG,
                                   TRUE, /* sync */
                                   pool));
    
  if (eb->switch_url)
    {
      /* The SVN_WC__LOG_DELETE_ENTRY log item will cause
       * svn_wc_remove_from_revision_control() to be run.  But that
       * function checks whether the deletion target's URL is child of
       * its parent directory's URL, and if it's not, then the entry
       * in parent won't be deleted (because presumably the child
       * represents a disjoint working copy, i.e., it is a wc_root).
       *
       * However, during a switch this works against us, because by
       * the time we get here, the parent's URL has already been
       * changed.  So we manually remove the child from revision
       * control after the delete-entry item has been written in the
       * parent's log, but before it is run, so the only work left for
       * the log item is to remove the entry in the parent directory.
       */

      if (kind == svn_node_dir)
        {
          svn_wc_adm_access_t *child_access;

          SVN_ERR (svn_wc_adm_retrieve
                   (&child_access, eb->adm_access,
                    full_path, pool));
          
          SVN_ERR (leftmod_error_chain 
                   (svn_wc_remove_from_revision_control 
                    (child_access,
                     SVN_WC_ENTRY_THIS_DIR,
                     TRUE, /* destroy */
                     TRUE, /* instant error */
                     eb->cancel_func,
                     eb->cancel_baton,
                     pool),
                    logfile_path, parent_path, pool));
        }
    }

  SVN_ERR (leftmod_error_chain (svn_wc__run_log (adm_access, NULL, pool),
                                logfile_path, parent_path, pool));

  /* The passed-in `path' is relative to the anchor of the edit, so if
   * the operation was invoked on something other than ".", then
   * `path' will be wrong for purposes of notification.  However, we
   * can always count on the parent_path being the parent of base_name,
   * so we just join them together to get a good notification path.
   */
  if (eb->notify_func)
    (*eb->notify_func) (eb->notify_baton,
                        svn_path_join (parent_path, base_name, pool),
                        svn_wc_notify_update_delete,
                        svn_node_unknown,
                        NULL,
                        svn_wc_notify_state_unknown,
                        svn_wc_notify_state_unknown,
                        SVN_INVALID_REVNUM);

  return SVN_NO_ERROR;
}


static svn_error_t *
delete_entry (const char *path, 
              svn_revnum_t revision, 
              void *parent_baton,
              apr_pool_t *pool)
{
  struct dir_baton *pb = parent_baton;
  return do_entry_deletion (pb->edit_baton, pb->path, path, pool);
}


static svn_error_t *
add_directory (const char *path,
               void *parent_baton,
               const char *copyfrom_path,
               svn_revnum_t copyfrom_revision,
               apr_pool_t *pool,
               void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct dir_baton *db = make_dir_baton (path, pb->edit_baton, pb, TRUE, pool);
  svn_node_kind_t kind;

  /* Semantic check.  Either both "copyfrom" args are valid, or they're
     NULL and SVN_INVALID_REVNUM.  A mixture is illegal semantics. */
  if ((copyfrom_path && (! SVN_IS_VALID_REVNUM (copyfrom_revision)))
      || ((! copyfrom_path) && (SVN_IS_VALID_REVNUM (copyfrom_revision))))
    abort();

  /* There should be nothing with this name. */
  SVN_ERR (svn_io_check_path (db->path, &kind, db->pool));
  if (kind != svn_node_none)
    return svn_error_createf
      (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
       "failed to add directory '%s': object of the same name already exists",
       db->path);

  /* It may not be named the same as the administrative directory. */
  if (strcmp (svn_path_basename (path, pool), SVN_WC_ADM_DIR_NAME) == 0)
    return svn_error_createf
      (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
       "failed to add directory '%s': \nobject of the same name as the "
       "administrative directory", db->path);

  /* Either we got real copyfrom args... */
  if (copyfrom_path || SVN_IS_VALID_REVNUM (copyfrom_revision))
    {
      /* ### todo: for now, this editor doesn't know how to deal with
         copyfrom args.  Someday it will interpet them as an update
         optimization, and actually copy one part of the wc to another.
         Then it will recursively "normalize" all the ancestry in the
         copied tree.  Someday! */      
      return svn_error_createf
        (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
         "failed to add directory '%s': copyfrom args not yet supported",
         db->path);
    }
  else  /* ...or we got invalid copyfrom args. */
    {
      svn_wc_adm_access_t *adm_access;
      const svn_wc_entry_t *dir_entry;
      apr_hash_t *entries;
      svn_wc_entry_t tmp_entry;

      /* Extra check:  a directory by this name may not exist, but there
         may still be one scheduled for addition.  That's a genuine
         tree-conflict.  */
      SVN_ERR (svn_wc_adm_retrieve (&adm_access, pb->edit_baton->adm_access,
                                    pb->path, db->pool));
      SVN_ERR (svn_wc_entries_read (&entries, adm_access, FALSE, db->pool));
      dir_entry = apr_hash_get (entries, db->name, APR_HASH_KEY_STRING);
      if (dir_entry && dir_entry->schedule == svn_wc_schedule_add)
        return svn_error_createf
          (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
           "failed to add dir '%s': \nobject of the same name is already "
           "scheduled for addition", path);

      /* Immediately create an entry for the new directory in the parent.
         Note that the parent must already be either added or opened, and
         thus it's in an 'incomplete' state just like the new dir.  */      
      tmp_entry.kind = svn_node_dir;
      /* (Note that there may already exist a 'ghost' entry in the
         parent with the same name, in a 'deleted' state.  If so, it's
         fine to overwrite it... but we need to make sure we get rid
         of the 'deleted' flag when doing so: */
      tmp_entry.deleted = FALSE;
      SVN_ERR (svn_wc__entry_modify (adm_access, db->name, &tmp_entry,
                                     SVN_WC__ENTRY_MODIFY_KIND |
                                     SVN_WC__ENTRY_MODIFY_DELETED,
                                     TRUE /* immediate write */, pool));
    }

  SVN_ERR (prep_directory (db,
                           db->new_URL,
                           pb->edit_baton->target_revision,
                           db->pool));

  *child_baton = db;

  if (db->edit_baton->notify_func)
    (*db->edit_baton->notify_func) (db->edit_baton->notify_baton,
                                    db->path,
                                    svn_wc_notify_update_add,
                                    svn_node_dir,
                                    NULL,
                                    svn_wc_notify_state_unknown,
                                    svn_wc_notify_state_unknown,
                                    SVN_INVALID_REVNUM);

  return SVN_NO_ERROR;
}


static svn_error_t *
open_directory (const char *path,
                void *parent_baton,
                svn_revnum_t base_revision,
                apr_pool_t *pool,
                void **child_baton)
{
  struct dir_baton *parent_dir_baton = parent_baton;
  struct edit_baton *eb = parent_dir_baton->edit_baton;
  svn_wc_entry_t tmp_entry;
  svn_wc_adm_access_t *adm_access;

  /* kff todo: check that the dir exists locally, find it somewhere if
     its not there?  Yes, all this and more...  And ancestor_url and
     ancestor_revision need to get used. */

  struct dir_baton *this_dir_baton = make_dir_baton (path, eb, 
                                                     parent_dir_baton, FALSE,
                                                     pool);
  *child_baton = this_dir_baton;

  /* Mark directory as being at target_revision and URL, but incomplete. */
  tmp_entry.revision = eb->target_revision;
  tmp_entry.url = this_dir_baton->new_URL;
  tmp_entry.incomplete = TRUE;

  SVN_ERR (svn_wc_adm_retrieve (&adm_access, eb->adm_access,
                                this_dir_baton->path, pool));  
  SVN_ERR (svn_wc__entry_modify (adm_access, NULL /* THIS_DIR */,
                                 &tmp_entry,
                                 SVN_WC__ENTRY_MODIFY_REVISION |
                                 SVN_WC__ENTRY_MODIFY_URL |
                                 SVN_WC__ENTRY_MODIFY_INCOMPLETE,
                                 TRUE /* immediate write */,
                                 pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
change_dir_prop (void *dir_baton,
                 const char *name,
                 const svn_string_t *value,
                 apr_pool_t *pool)
{
  svn_prop_t *propchange;
  struct dir_baton *db = dir_baton;

  propchange = apr_array_push (db->propchanges);
  propchange->name = apr_pstrdup (db->pool, name);
  propchange->value = value ? svn_string_dup (value, db->pool) : NULL;

  return SVN_NO_ERROR;
}



/* If any of the svn_prop_t objects in PROPCHANGES represents a change
   to the SVN_PROP_EXTERNALS property, return that change, else return
   null.  If PROPCHANGES contains more than one such change, return
   the first. */
static const svn_prop_t *
externals_prop_changed (apr_array_header_t *propchanges)
{
  int i;

  for (i = 0; i < propchanges->nelts; i++)
    {
      const svn_prop_t *p = &(APR_ARRAY_IDX(propchanges, i, svn_prop_t));
      if (strcmp (p->name, SVN_PROP_EXTERNALS) == 0)
        return p;
    }

  return NULL;
}

static svn_error_t *
close_directory (void *dir_baton,
                 apr_pool_t *pool)
{
  struct dir_baton *db = dir_baton;
  svn_wc_notify_state_t prop_state = svn_wc_notify_state_unknown;
  apr_array_header_t *entry_props, *wc_props, *regular_props;

  SVN_ERR (svn_categorize_props (db->propchanges, &entry_props, &wc_props,
                                 &regular_props, pool));

  /* If this directory has property changes stored up, now is the time
     to deal with them. */
  if (regular_props->nelts || entry_props->nelts || wc_props->nelts)
    {
      svn_wc_adm_access_t *adm_access;
      apr_file_t *log_fp = NULL;
      apr_status_t apr_err;

      /* to hold log messages: */
      svn_stringbuf_t *entry_accum = svn_stringbuf_create ("", db->pool);

      SVN_ERR (svn_wc_adm_retrieve (&adm_access, db->edit_baton->adm_access,
                                    db->path, db->pool));
      
      /* Open log file */
      SVN_ERR (svn_wc__open_adm_file (&log_fp,
                                      db->path,
                                      SVN_WC__ADM_LOG,
                                      (APR_WRITE | APR_CREATE), /* not excl */
                                      db->pool));

      if (regular_props->nelts)
        {
          svn_boolean_t prop_modified;

          /* If recording traversal info, then see if the
             SVN_PROP_EXTERNALS property on this directory changed,
             and record before and after for the change. */
          if (db->edit_baton->traversal_info)
            {
              svn_wc_traversal_info_t *ti = db->edit_baton->traversal_info;
              const svn_prop_t *change = externals_prop_changed (regular_props);

              if (change)
                {
                  const svn_string_t *new_val_s = change->value;
                  const svn_string_t *old_val_s;

                  SVN_ERR (svn_wc_prop_get
                           (&old_val_s, SVN_PROP_EXTERNALS,
                            db->path, adm_access, db->pool));

                  if ((new_val_s == NULL) && (old_val_s == NULL))
                    ; /* No value before, no value after... so do nothing. */
                  else if (new_val_s && old_val_s
                           && (svn_string_compare (old_val_s, new_val_s)))
                    ; /* Value did not change... so do nothing. */
                  else  /* something changed, record the change */
                    {
                      /* We can't assume that ti came pre-loaded with the
                         old values of the svn:externals property.  Yes,
                         most callers will have already initialized ti by
                         sending it through svn_wc_crawl_revisions, but we
                         shouldn't count on that here -- so we set both the
                         old and new values again. */

                      if (old_val_s)
                        apr_hash_set (ti->externals_old,
                                      apr_pstrdup (ti->pool, db->path),
                                      APR_HASH_KEY_STRING,
                                      apr_pstrmemdup (ti->pool, old_val_s->data,
                                                      old_val_s->len));

                      if (new_val_s)
                        apr_hash_set (ti->externals_new,
                                      apr_pstrdup (ti->pool, db->path),
                                      APR_HASH_KEY_STRING,
                                      apr_pstrmemdup (ti->pool, new_val_s->data,
                                                      new_val_s->len));
                    }
                }
            }

          /* Merge pending properties into temporary files (ignoring
             conflicts). */
          SVN_ERR_W (svn_wc__merge_prop_diffs (&prop_state,
                                               adm_access, NULL,
                                               regular_props, TRUE, FALSE,
                                               db->pool, &entry_accum),
                     "couldn't do prop merge.");

          /* Are the directory's props locally modified? */
          SVN_ERR (svn_wc_props_modified_p (&prop_modified,
                                            db->path, adm_access,
                                            db->pool));

          /* Log entry which sets a new property timestamp, but *only* if
             there are no local changes to the props. */
          if (! prop_modified)
            svn_xml_make_open_tag (&entry_accum,
                                   db->pool,
                                   svn_xml_self_closing,
                                   SVN_WC__LOG_MODIFY_ENTRY,
                                   SVN_WC__LOG_ATTR_NAME,
                                   SVN_WC_ENTRY_THIS_DIR,
                                   SVN_WC__ENTRY_ATTR_PROP_TIME,
                                   /* use wfile time */
                                   SVN_WC_TIMESTAMP_WC,
                                   NULL);
        }

      accumulate_entry_props (entry_accum, SVN_WC_ENTRY_THIS_DIR, entry_props,
                              pool);

      accumulate_wcprops (entry_accum, SVN_WC_ENTRY_THIS_DIR, wc_props, pool);

      /* Write our accumulation of log entries into a log file */
      apr_err = apr_file_write_full (log_fp, entry_accum->data,
                                     entry_accum->len, NULL);
      if (apr_err)
        {
          apr_file_close (log_fp);
          return svn_error_createf (apr_err, NULL,
                                    "error writing log file for '%s'",
                                    db->path);
        }

      /* The log is ready to run, close it. */
      SVN_ERR (svn_wc__close_adm_file (log_fp,
                                       db->path,
                                       SVN_WC__ADM_LOG,
                                       TRUE, /* sync */
                                       db->pool));

      /* Run the log. */
      SVN_ERR (svn_wc__run_log (adm_access, NULL, db->pool));
    }

  /* We're done with this directory, so remove one reference from the
     bump information. This may trigger a number of actions. See
     maybe_bump_dir_info() for more information.  */
  SVN_ERR (maybe_bump_dir_info (db->edit_baton, db->bump_info, db->pool));

  /* Notify of any prop changes on this directory -- but do nothing
     if it's an added directory, because notification has already
     happened in that case. */
  if ((! db->added) && (db->edit_baton->notify_func))
    (*db->edit_baton->notify_func) (db->edit_baton->notify_baton,
                                    db->path,
                                    svn_wc_notify_update_update,
                                    svn_node_dir,
                                    NULL,
                                    svn_wc_notify_state_unknown,
                                    prop_state,
                                    SVN_INVALID_REVNUM);

  return SVN_NO_ERROR;
}



/* Common code for add_file() and open_file(). */
static svn_error_t *
add_or_open_file (const char *path,
                  void *parent_baton,
                  const char *copyfrom_path,
                  svn_revnum_t copyfrom_rev,
                  void **file_baton,
                  svn_boolean_t adding, /* 0 if replacing */
                  apr_pool_t *pool)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *fb;
  const svn_wc_entry_t *entry;
  svn_node_kind_t kind;
  svn_wc_adm_access_t *adm_access;

  /* the file_pool can stick around for a *long* time, so we want to use
     a subpool for any temporary allocations. */
  apr_pool_t *subpool = svn_pool_create (pool);

  /* ### kff todo: if file is marked as removed by user, then flag a
     conflict in the entry and proceed.  Similarly if it has changed
     kind.  see issuezilla task #398. */

  fb = make_file_baton (pb, path, adding, pool);

  /* It is interesting to note: everything below is just validation. We
     aren't actually doing any "work" or fetching any persistent data. */

  SVN_ERR (svn_io_check_path (fb->path, &kind, subpool));
  SVN_ERR (svn_wc_adm_retrieve (&adm_access, pb->edit_baton->adm_access,
                                pb->path, subpool));
  SVN_ERR (svn_wc_entry (&entry, fb->path, adm_access, FALSE, subpool));
  
  /* Sanity checks. */

  /* If adding, there should be nothing with this name. */
  if (adding && (kind != svn_node_none))
    return svn_error_createf
      (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
       "failed to add file '%s': object of the same name already exists",
       fb->path);

  /* sussman sez: If we're trying to add a file that's already in
     `entries' (but not on disk), that's okay.  It's probably because
     the user deleted the working version and ran 'svn up' as a means
     of getting the file back.  

     It certainly doesn't hurt to re-add the file.  We can't possibly
     get the entry showing up twice in `entries', since it's a hash;
     and we know that we won't lose any local mods.  Let the existing
     entry be overwritten.

     sussman follows up to himself, many months later: the above
     scenario is fine, as long as the pre-existing entry isn't
     scheduled for addition.  that's a genuine tree-conflict,
     regardless of whether the working file still exists.  */

  if (adding && entry && entry->schedule == svn_wc_schedule_add)
    return svn_error_createf
      (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
       "failed to add file '%s': \nobject of the same name is already "
       "scheduled for addition", fb->path);
    

  /* If replacing, make sure the .svn entry already exists. */
  if ((! adding) && (! entry))
    return svn_error_createf (SVN_ERR_ENTRY_NOT_FOUND, NULL,
                              "trying to open non-versioned file "
                              "'%s' in directory '%s'",
                              fb->name, pb->path);
  
  /* ### todo:  right now the incoming copyfrom* args are being
     completely ignored!  Someday the editor-driver may expect us to
     support this optimization;  when that happens, this func needs to
     -copy- the specified existing wc file to this location.  From
     there, the driver can apply_textdelta on it, etc. */

  svn_pool_destroy (subpool);

  *file_baton = fb;
  return SVN_NO_ERROR;
}


static svn_error_t *
add_file (const char *name,
          void *parent_baton,
          const char *copyfrom_path,
          svn_revnum_t copyfrom_revision,
          apr_pool_t *pool,
          void **file_baton)
{
  return add_or_open_file (name, parent_baton, copyfrom_path, 
                           copyfrom_revision, file_baton, TRUE, pool);
}


static svn_error_t *
open_file (const char *name,
           void *parent_baton,
           svn_revnum_t base_revision,
           apr_pool_t *pool,
           void **file_baton)
{
  return add_or_open_file (name, parent_baton, NULL, base_revision, 
                           file_baton, FALSE, pool);
}


static svn_error_t *
apply_textdelta (void *file_baton, 
                 const char *base_checksum,
                 apr_pool_t *pool,
                 svn_txdelta_window_handler_t *handler,
                 void **handler_baton)
{
  struct file_baton *fb = file_baton;
  apr_pool_t *handler_pool = svn_pool_create (fb->pool);
  struct handler_baton *hb = apr_palloc (handler_pool, sizeof (*hb));
  svn_error_t *err;
  svn_wc_adm_access_t *adm_access;
  const svn_wc_entry_t *ent;

  /* Open the text base for reading, unless this is a checkout. */
  hb->source = NULL;

  /* 
     kff todo: what we really need to do here is:
       
     1. See if there's a file or dir by this name already here.
     2. See if it's under revision control.
     3. If both are true, open text-base.
     4. If only 1 is true, bail, because we can't go destroying user's
        files (or as an alternative to bailing, move it to some tmp
        name and somehow tell the user, but communicating with the
        user without erroring is a whole callback system we haven't
        finished inventing yet.)
  */

  /* Before applying incoming svndiff data to text base, make sure
     text base hasn't been corrupted, and that its checksum
     matches the expected base checksum. */
  SVN_ERR (svn_wc_adm_retrieve (&adm_access, fb->edit_baton->adm_access,
                                svn_path_dirname (fb->path, pool), pool));
  SVN_ERR (svn_wc_entry (&ent, fb->path, adm_access, FALSE, pool));
      
  /* Only compare checksums this file has an entry, and the entry has
     a checksum.  If there's no entry, it just means the file is
     created in this update, so there won't be any previously recorded
     checksum to compare against.  If no checksum, well, for backwards
     compatibility we assume that no checksum always matches. */
  if (ent && ent->checksum)
    {
      unsigned char digest[APR_MD5_DIGESTSIZE];
      const char *hex_digest;
      const char *tb;
      
      tb = svn_wc__text_base_path (fb->path, FALSE, pool);
      SVN_ERR (svn_io_file_checksum (digest, tb, pool));
      hex_digest = svn_md5_digest_to_cstring (digest, pool);
      
      /* Compare the base_checksum here, rather than in the window
         handler, because there's no guarantee that the handler will
         see every byte of the base file. */
      if (base_checksum)
        {
          if (strcmp (hex_digest, base_checksum) != 0)
            return svn_error_createf
              (SVN_ERR_WC_CORRUPT_TEXT_BASE, NULL,
               "apply_textdelta: checksum mismatch for '%s':\n"
               "   expected checksum:  %s\n"
               "   actual checksum:    %s\n",
               tb, base_checksum, hex_digest);
        }
      
      if (strcmp (hex_digest, ent->checksum) != 0)
        {
          /* Compatibility hack: working copies created before 13 Jan
             2003 may have entry checksums stored in base64.  See
             svn_io_file_checksum_base64()'s doc string for
             details. */ 
          const char *base64_digest 
            = (svn_base64_from_md5 (digest, pool))->data;
              
          if (strcmp (base64_digest, ent->checksum) != 0)
            {
              return svn_error_createf
                (SVN_ERR_WC_CORRUPT_TEXT_BASE, NULL,
                 "apply_textdelta: checksum mismatch for '%s':\n"
                 "   recorded checksum:        %s\n"
                 "   actual checksum (hex):    %s\n"
                 "   actual checksum (base64): %s\n",
                 tb, ent->checksum, hex_digest, base64_digest);
            }
        }
    }
  
  err = svn_wc__open_text_base (&hb->source, fb->path, APR_READ,
                                handler_pool);
  if (err && !APR_STATUS_IS_ENOENT(err->apr_err))
    {
      if (hb->source)
        {
          svn_error_t *err2 = svn_wc__close_text_base (hb->source,
                                                       fb->path,
                                                       0, handler_pool);
          if (err2)
            svn_error_clear (err2);
        }
      svn_pool_destroy (handler_pool);
      return err;
    }
  else if (err)
    {
      svn_error_clear (err);
      hb->source = NULL;  /* make sure */
    }
  
  /* Open the text base for writing (this will get us a temporary file).  */
  hb->dest = NULL;
  err = svn_wc__open_text_base (&hb->dest, fb->path,
                                (APR_WRITE | APR_TRUNCATE | APR_CREATE),
                                handler_pool);
  if (err)
    {
      if (hb->dest)
        svn_wc__close_text_base (hb->dest, fb->path, 0, handler_pool);
      svn_pool_destroy (handler_pool);
      return err;
    }
  
  /* Prepare to apply the delta.  */
  {
    const char *tmp_path;

    apr_file_name_get (&tmp_path, hb->dest);
    svn_txdelta_apply (svn_stream_from_aprfile (hb->source, handler_pool),
                       svn_stream_from_aprfile (hb->dest, handler_pool),
                       fb->digest, tmp_path, handler_pool,
                       &hb->apply_handler, &hb->apply_baton);
  }
  
  hb->pool = handler_pool;
  hb->fb = fb;
  
  /* We're all set.  */
  *handler_baton = hb;
  *handler = window_handler;

  return SVN_NO_ERROR;
}




static svn_error_t *
change_file_prop (void *file_baton,
                  const char *name,
                  const svn_string_t *value,
                  apr_pool_t *pool)
{
  struct file_baton *fb = file_baton;
  svn_prop_t *propchange;

  /* Push a new propchange to the file baton's array of propchanges */
  propchange = apr_array_push (fb->propchanges);
  propchange->name = apr_pstrdup (fb->pool, name);
  propchange->value = value ? svn_string_dup (value, fb->pool) : NULL;

  /* Let close_file() know that propchanges are waiting to be
     applied. */
  fb->prop_changed = 1;

  /* Special case: if the file is added during a checkout, cache the
     last-changed-date propval for future use. */
  if (fb->edit_baton->use_commit_times
      && (strcmp (name, SVN_PROP_ENTRY_COMMITTED_DATE) == 0))
    fb->last_changed_date = apr_pstrdup (fb->pool, value->data);

  return SVN_NO_ERROR;
}



/* This is the small planet.  It has the complex responsibility of
 * "integrating" a new revision of a file into a working copy. 
 *
 * Given a @a file_path either already under version control, or
 * prepared (see below) to join revision control, fully install a @a
 * new_revision of the file; @a new_revision must be 0 if copyfrom
 * args are passed, see below for details.  @a adm_access is an access
 * baton with a write lock for the directory containing @a file_path.
 *
 * If @a file_path is not already under version control (i.e., does
 * not have an entry), then the raw data (for example the new text
 * base and new props) required to put it under version control must
 * be provided by the caller.  See below for details.
 *
 * By "install", we mean: create a new text-base and prop-base, merge
 * any textual and property changes into the working file, and finally
 * update all metadata so that the working copy believes it has a new
 * working revision of the file.  All of this work includes being
 * sensitive to eol translation, keyword substitution, and performing
 * all actions using a journaled logfile.
 *
 * The caller provides a @a new_text_path which points to a temporary
 * file containing the 'new' base text of the file at revision @a
 * new_revision.  This function automatically removes @a new_text_path
 * upon successful completion.  If there is no new text, then caller
 * must set @a new_text_path to @c NULL.
 *
 * The caller also provides the new properties for the file in the
 * @a props array; if there are no new props, then caller must pass 
 * @c NULL instead.  This argument is an array of @c svn_prop_t structures, 
 * and can be interpreted in one of two ways:
 *
 *    - if @a is_full_proplist is true, then the array represents the
 *      complete list of all properties for the file.  It is the new
 *      'pristine' proplist.
 *
 *    - if @a is_full_proplist is false, then the array represents a set of
 *      *differences* against the file's existing pristine proplist.
 *      (A deletion is represented by setting an @c svn_prop_t's 'value'
 *      field to @c NULL.)  
 *
 * Note that the @a props array is expected to contain all categories of
 * props, not just 'regular' ones that the user sees.  (See <tt>enum
 * svn_prop_kind</tt>).
 *
 * If @a content_state is non-null, set @a *content_state to the state of
 * the file contents after the installation; if return error, the
 * value of @a *content_state is undefined.
 *
 * If @a prop_state is non-null, set @a *prop_state to the state of the
 * properties after the installation; if return error, the value of
 * @a *prop_state is undefined.
 *
 * If @a new_url is non-null, then this URL will be attached to the file
 * in the 'entries' file.  Otherwise, the file will simply "inherit"
 * its URL from the parent dir.
 *
 * If @a is_add is true, schedule the new file for addition; and if
 * @a copyfrom_url is non-null, then store it and @a copyfrom_rev as
 * copyfrom history for the added file.  An assertion error may occur
 * if copyfrom args while @a is_add is false.  An assertion error may
 * occur if @a copyfrom_url is non-null but @a copyfrom_rev is not a
 * valid revision number.  An assertion error may occur if @a is_add
 * is true but @a new_revision is not 0.
 *
 * If @a diff3_cmd is non-null, then use it as the diff3 command for
 * any merging; otherwise, use the built-in merge code.
 *
 * If @a timestamp_string is non-null, then use it to set the
 * timestamp on the final working file.  The string should be
 * formatted for use by svn_time_from_cstring().
 *
 * @a pool is used for all bookkeeping work during the installation.
 */
static svn_error_t *
install_file (svn_wc_notify_state_t *content_state,
              svn_wc_notify_state_t *prop_state,
              svn_wc_adm_access_t *adm_access,
              const char *file_path,
              svn_revnum_t new_revision,
              const char *new_text_path,
              const apr_array_header_t *props,
              svn_boolean_t is_full_proplist,
              const char *new_URL,
              svn_boolean_t is_add,
              const char *copyfrom_url,
              svn_revnum_t copyfrom_rev,
              const char *diff3_cmd,
              const char *timestamp_string,
              apr_pool_t *pool)
{
  apr_file_t *log_fp = NULL;
  apr_status_t apr_err;
  char *revision_str = NULL;
  const char *parent_dir, *base_name;
  svn_stringbuf_t *log_accum;
  svn_boolean_t is_locally_modified;
  svn_boolean_t magic_props_changed = FALSE;
  apr_array_header_t *regular_props = NULL, *wc_props = NULL,
    *entry_props = NULL;

  /* The code flow does not depend upon these being set to NULL, but
     it removes a gcc 3.1 `might be used uninitialized in this
     function' warning. */
  const char *txtb = NULL, *tmp_txtb = NULL;

  /* Start by splitting FILE_PATH. */
  svn_path_split (file_path, &parent_dir, &base_name, pool);

  /*
     When this function is called on file F, we assume the following
     things are true:

         - The new pristine text of F, if any, is present at
           NEW_TEXT_PATH.           

         - The .svn/entries file still reflects the old version of F.

         - .svn/text-base/F.svn-base is the old pristine F.

      The goal is to update the local working copy of F to reflect
      the changes received from the repository, preserving any local
      modifications, in an interrupt-safe way.  So we first write our
      intentions to .svn/log, then run over the log file doing each
      operation in turn.  For a given operation, you can tell by
      inspection whether or not it has already been done; thus, those
      that have already been done are no-ops, and when we reach the
      end of the log file, we remove it.
  */

  /* Open a log file.  This is safe because the adm area is locked
     right now. */
  SVN_ERR (svn_wc__open_adm_file (&log_fp,
                                  parent_dir,
                                  SVN_WC__ADM_LOG,
                                  (APR_WRITE | APR_CREATE), /* not excl */
                                  pool));

  /* Accumulate log commands in this buffer until we're ready to close
     and run the log.  */
  log_accum = svn_stringbuf_create ("", pool);
  
  /* If we need to schedule this for addition, do it first, before the
   * entry exists.  Otherwise we'll get bounced out with an error
   * about scheduling an already-versioned item for addition.
   */
  if (is_add)
    {
      const char *rev_str;
      
      assert (new_revision == 0);

      if (copyfrom_url)
        {
          assert (SVN_IS_VALID_REVNUM (copyfrom_rev));
          rev_str = apr_psprintf (pool, "%" SVN_REVNUM_T_FMT, copyfrom_rev);
        }

      svn_xml_make_open_tag
        (&log_accum,
         pool,
         svn_xml_self_closing,
         SVN_WC__LOG_MODIFY_ENTRY,
         SVN_WC__LOG_ATTR_NAME,
         base_name,
         SVN_WC__ENTRY_ATTR_SCHEDULE,
         SVN_WC__ENTRY_VALUE_ADD,
         copyfrom_url ? SVN_WC__ENTRY_ATTR_COPYFROM_URL : NULL,
         copyfrom_url,
         SVN_WC__ENTRY_ATTR_COPYFROM_REV,
         rev_str,
         SVN_WC__ENTRY_ATTR_COPIED,
         "true",
         NULL);
    }
  else
    {
      assert (copyfrom_url == NULL);
    }

  /* Log commands can only operate on paths that are below the
     parent_dir.  Thus if NEW_TEXT_PATH is somewhere *outside* of
     FILE_PATH's parent directory, we can't write a log command to do
     a move from one location to another.  So the solution, then, is
     to simply move NEW_TEXT_PATH to .svn/tmp/text-base/ immediately
     -- that's where the rest of this code wants it to be anyway. */
  if (new_text_path)
    {
      const char *final_location = 
        svn_wc__text_base_path (file_path, TRUE, pool);
      
      /* Only do the 'move' if NEW_TEXT_PATH isn't -already-
         pointing to parent_dir/.svn/tmp/text-base/basename.  */
      if (strcmp (final_location, new_text_path))
        {
          SVN_ERR_W (svn_io_file_rename (new_text_path, final_location,
                                         pool),
                     "install_file: move failed");

          new_text_path = final_location;
        }
    }
  
  /* Sort the property list into three arrays, based on kind. */
  if (props)
    SVN_ERR (svn_categorize_props (props,
                                   &entry_props, &wc_props, &regular_props,
                                   pool));

  /* Always initialize to unknown state. */
  if (prop_state)
    *prop_state = svn_wc_notify_state_unknown;

  /* Merge the 'regular' props into the existing working proplist. */
  if (regular_props)
    {
      apr_array_header_t *propchanges;
      apr_hash_t *old_pristine_props, *new_pristine_props;
      
      if (is_full_proplist)
        {         
          /* If the caller passed a definitive list that represents all
             of the file's properties, we need to compare it to the
             current 'pristine' list and deduce the differences. */
          const char *pristine_prop_path;
          int i;
          old_pristine_props = apr_hash_make (pool);
          new_pristine_props = apr_hash_make (pool);
          
          /* Get the current pristine props. */
          SVN_ERR (svn_wc__prop_base_path (&pristine_prop_path,
                                           file_path, adm_access, FALSE, pool));
          SVN_ERR (svn_wc__load_prop_file (pristine_prop_path,
                                           old_pristine_props, pool));
          
          /* Convert the given array into hash of 'new' pristine props. */
          for (i = 0; i < regular_props->nelts; i++)
            {
              const svn_prop_t *prop = NULL;
              prop = &APR_ARRAY_IDX (regular_props, i, svn_prop_t);
              apr_hash_set (new_pristine_props,
                            prop->name, APR_HASH_KEY_STRING, prop->value);
            }
          
          /* Deduce changes. */
          SVN_ERR (svn_prop_diffs (&propchanges, new_pristine_props,
                                   old_pristine_props, pool));
        }
      else
        /* The user gave us a list prop diffs directly, yay. */
        propchanges = regular_props;
      
      /* Now that we have the list of diffs... */
      
      /* Determine if any of the propchanges are the "magic" ones that
         might require changing the working file. */
      {
        int i;
        for (i = 0; i < propchanges->nelts; i++)
          {
            svn_prop_t *propchange
              = &APR_ARRAY_IDX (propchanges, i, svn_prop_t);
            
            if ((! strcmp (propchange->name, SVN_PROP_EXECUTABLE))
                || (! strcmp (propchange->name, SVN_PROP_KEYWORDS))
                || (! strcmp (propchange->name, SVN_PROP_EOL_STYLE)))
              magic_props_changed = TRUE;
          }
      }

      /* This will merge the old and new props into a new prop db, and
         write <cp> commands to the logfile to install the merged
         props.  */
      SVN_ERR (svn_wc__merge_prop_diffs (prop_state,
                                         adm_access, base_name,
                                         propchanges, TRUE, FALSE, pool,
                                         &log_accum));
    }
  
  /* If there are any ENTRY PROPS, make sure those get appended to the
     growing log as fields for the file's entry.  This needs to happen
     before we do any textual merging, because that process might
     expand keywords, and we want the keyword info to be up-to-date.

     Note that no merging needs to happen; these kinds of props aren't
     versioned, so the value of IS_FULL_PROPLIST is irrelevant -- if
     the property is present, we overwrite the value. */  
  if (entry_props)
    accumulate_entry_props (log_accum, base_name, entry_props, pool);

  /* Has the user made local mods to the working file?  */
  SVN_ERR (svn_wc_text_modified_p (&is_locally_modified,
                                   file_path, FALSE, adm_access, pool));

  if (new_text_path)   /* is there a new text-base to install? */
    {
      txtb     = svn_wc__text_base_path (base_name, FALSE, pool);
      tmp_txtb = svn_wc__text_base_path (base_name, TRUE, pool);
    }
  else if (magic_props_changed) /* no new text base, but... */
    {
      /* Special edge-case: it's possible that this file installation
         only involves propchanges, but that some of those props still
         require a retranslation of the working file. */

      const char *tmptext = svn_wc__text_base_path (base_name, TRUE, pool);

      /* A log command which copies and DEtranslates the working file
         to a tmp-text-base. */
      svn_xml_make_open_tag (&log_accum, pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_CP_AND_DETRANSLATE,
                             SVN_WC__LOG_ATTR_NAME, base_name,
                             SVN_WC__LOG_ATTR_DEST, tmptext,
                             NULL);

      /* A log command that copies the tmp-text-base and REtranslates
         the tmp-text-base back to the working file. */
      svn_xml_make_open_tag (&log_accum, pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_CP_AND_TRANSLATE,
                             SVN_WC__LOG_ATTR_NAME, tmptext,
                             SVN_WC__LOG_ATTR_DEST, base_name,
                             NULL);
    }

  /* Write log entry which will bump the revision number.  Also, just
     in case we're overwriting an existing phantom 'deleted' entry, be
     sure to remove the deleted-ness. */
  revision_str = apr_psprintf (pool, "%" SVN_REVNUM_T_FMT, new_revision);
  svn_xml_make_open_tag (&log_accum,
                         pool,
                         svn_xml_self_closing,
                         SVN_WC__LOG_MODIFY_ENTRY,
                         SVN_WC__LOG_ATTR_NAME,
                         base_name,
                         SVN_WC__ENTRY_ATTR_KIND,
                         SVN_WC__ENTRIES_ATTR_FILE_STR,
                         SVN_WC__ENTRY_ATTR_REVISION,
                         revision_str,
                         SVN_WC__ENTRY_ATTR_DELETED,
                         "false",
                         NULL);


  /* Possibly install a *non*-inherited URL in the entry. */
  if (new_URL)
    {
      svn_xml_make_open_tag (&log_accum,
                             pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_MODIFY_ENTRY,
                             SVN_WC__LOG_ATTR_NAME,
                             base_name,
                             SVN_WC__ENTRY_ATTR_URL,
                             new_URL,
                             NULL);
    }

  /* For 'textual' merging, we implement this matrix.

                  Text file                   Binary File
               -----------------------------------------------
    Local Mods | svn_wc_merge uses diff3, | svn_wc_merge     |
               | possibly makes backups & | makes backups,   |
               | marks file as conflicted.| marks conflicted |
               -----------------------------------------------
    No Mods    |        Just overwrite working file.         |
               |                                             |
               -----------------------------------------------

   So the first thing we do is figure out where we are in the
   matrix. */
  if (new_text_path)
    {
      if (! is_locally_modified)
        {
          /* If there are no local mods, who cares whether it's a text
             or binary file!  Just write a log command to overwrite
             any working file with the new text-base.  If newline
             conversion or keyword substitution is activated, this
             will happen as well during the copy. */
          svn_xml_make_open_tag (&log_accum,
                                 pool,
                                 svn_xml_self_closing,
                                 SVN_WC__LOG_CP_AND_TRANSLATE,
                                 SVN_WC__LOG_ATTR_NAME,
                                 tmp_txtb,
                                 SVN_WC__LOG_ATTR_DEST,
                                 base_name,
                                 NULL);
        }
  
      else   /* working file is locally modified... */
        {
          svn_node_kind_t wfile_kind = svn_node_unknown;
          
          SVN_ERR (svn_io_check_path (file_path, &wfile_kind, pool));
          if (wfile_kind == svn_node_none) /* working file is missing?! */
            {
              /* Just copy the new text-base to the file. */
              svn_xml_make_open_tag (&log_accum,
                                     pool,
                                     svn_xml_self_closing,
                                     SVN_WC__LOG_CP_AND_TRANSLATE,
                                     SVN_WC__LOG_ATTR_NAME,
                                     tmp_txtb,
                                     SVN_WC__LOG_ATTR_DEST,
                                     base_name,
                                     NULL);
            }
          else  /* working file exists, and has local mods.*/
            {                  
              /* Now we need to let loose svn_wc_merge() to merge the
                 textual changes into the working file. */
              const char *oldrev_str, *newrev_str;
              const svn_wc_entry_t *e;
              
              /* Create strings representing the revisions of the
                 old and new text-bases. */
              SVN_ERR (svn_wc_entry (&e, file_path, adm_access, FALSE, pool));
              assert (e != NULL);
              oldrev_str = apr_psprintf (pool, ".r%" SVN_REVNUM_T_FMT,
                                         e->revision);
              newrev_str = apr_psprintf (pool, ".r%" SVN_REVNUM_T_FMT,
                                         new_revision);
              
              /* Merge the changes from the old-textbase (TXTB) to
                 new-textbase (TMP_TXTB) into the file we're
                 updating (BASE_NAME).  Either the merge will
                 happen smoothly, or a conflict will result.
                 Luckily, this routine will take care of all eol
                 and keyword translation, and diff3 will insert
                 conflict markers for us.  It also deals with binary
                 files appropriately.  */
              svn_xml_make_open_tag (&log_accum,
                                     pool,
                                     svn_xml_self_closing,
                                     SVN_WC__LOG_MERGE,
                                     SVN_WC__LOG_ATTR_NAME, base_name,
                                     SVN_WC__LOG_ATTR_ARG_1, txtb,
                                     SVN_WC__LOG_ATTR_ARG_2, tmp_txtb,
                                     SVN_WC__LOG_ATTR_ARG_3, oldrev_str,
                                     SVN_WC__LOG_ATTR_ARG_4, newrev_str,
                                     SVN_WC__LOG_ATTR_ARG_5, ".mine",
                                     NULL);
              
              /* If a conflict happens, then the entry will be
                 marked "Conflicted" and will track either 2 or 3 new
                 temporary fulltext files that resulted. */
              
            } /* end: working file exists and has mods */
        } /* end: working file has mods */
    }  /* end:  "textual" merging process */

  /* Possibly write log commands to tweak text/prop entry timestamps: */
  if ((new_text_path) || (magic_props_changed))
    {
      /* Log entry which sets a new textual timestamp, but only if
         there are no local changes to the text. */
      if (! is_locally_modified)
        svn_xml_make_open_tag (&log_accum,
                               pool,
                               svn_xml_self_closing,
                               SVN_WC__LOG_MODIFY_ENTRY,
                               SVN_WC__LOG_ATTR_NAME,
                               base_name,
                               SVN_WC__ENTRY_ATTR_TEXT_TIME,
                               /* use wfile time */
                               SVN_WC_TIMESTAMP_WC,
                               NULL);
    }

  if (props)
    {
      svn_boolean_t prop_modified;

      /* Are the working file's props locally modified? */
      SVN_ERR (svn_wc_props_modified_p (&prop_modified,
                                        file_path, adm_access,
                                        pool));

      /* Log entry which sets a new property timestamp, but only if
         there are no local changes to the props. */
      if (! prop_modified)
        svn_xml_make_open_tag (&log_accum,
                               pool,
                               svn_xml_self_closing,
                               SVN_WC__LOG_MODIFY_ENTRY,
                               SVN_WC__LOG_ATTR_NAME,
                               base_name,
                               SVN_WC__ENTRY_ATTR_PROP_TIME,
                               /* use wfile time */
                               SVN_WC_TIMESTAMP_WC,
                               NULL);
    }

  if (new_text_path)
    {
      /* Write out log commands to set up the new text base and its
         checksum. */

      svn_xml_make_open_tag (&log_accum,
                             pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_MV,
                             SVN_WC__LOG_ATTR_NAME,
                             tmp_txtb,
                             SVN_WC__LOG_ATTR_DEST,
                             txtb,
                             NULL);
      
      svn_xml_make_open_tag (&log_accum,
                             pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_READONLY,
                             SVN_WC__LOG_ATTR_NAME,
                             txtb,
                             NULL);

      {
        unsigned char digest[APR_MD5_DIGESTSIZE];
        SVN_ERR (svn_io_file_checksum (digest, new_text_path, pool));
        svn_xml_make_open_tag (&log_accum,
                               pool,
                               svn_xml_self_closing,
                               SVN_WC__LOG_MODIFY_ENTRY,
                               SVN_WC__LOG_ATTR_NAME,
                               base_name,
                               SVN_WC__ENTRY_ATTR_CHECKSUM,
                               svn_md5_digest_to_cstring (digest, pool),
                               NULL);
      }
    }

  /* This writes a whole bunch of log commands to install entryprops.  */
  if (wc_props)
    accumulate_wcprops (log_accum, base_name, wc_props, pool);

  /* Possibly write a log command to set timestamp on the final
     working file.  This command should be LAST in the logfile! */
  if (timestamp_string)
    svn_xml_make_open_tag (&log_accum,
                           pool,
                           svn_xml_self_closing,
                           SVN_WC__LOG_SET_TIMESTAMP,
                           SVN_WC__LOG_ATTR_NAME,
                           base_name,
                           SVN_WC__LOG_ATTR_TIMESTAMP,
                           timestamp_string,
                           NULL);

  /* Write our accumulation of log entries into a log file */
  apr_err = apr_file_write_full (log_fp, log_accum->data, 
                                 log_accum->len, NULL);
  if (apr_err)
    {
      apr_file_close (log_fp);
      return svn_error_createf (apr_err, NULL,
                                "error writing log for '%s'.",
                                file_path);
    }

  /* The log is ready to run.  Close it and run it! */
  SVN_ERR (svn_wc__close_adm_file (log_fp, parent_dir, SVN_WC__ADM_LOG,
                                   TRUE, /* sync */ pool));
  SVN_ERR (svn_wc__run_log (adm_access, diff3_cmd, pool));

  if (content_state)
    {
      const svn_wc_entry_t *entry;
      svn_boolean_t tc, pc;

      /* Initialize the state of our returned value. */
      *content_state = svn_wc_notify_state_unknown;
      
      /* ### There should be a more efficient way of finding out whether
         or not the file is modified|merged|conflicted.  If the
         svn_wc__run_log() call above could return a special error code
         in case of a conflict or something, that would work. */

      SVN_ERR (svn_wc_entry (&entry, file_path, adm_access, TRUE, pool));
      SVN_ERR (svn_wc_conflicted_p (&tc, &pc, parent_dir, entry, pool));
      
      /* This is kind of interesting.  Even if no new text was
         installed (i.e., new_text_path was null), we could still
         report a pre-existing conflict state.  Say a file, already
         in a state of textual conflict, receives prop mods during an
         update.  Then we'll notify that it has text conflicts.  This
         seems okay to me.  I guess.  I dunno.  You? */

      if (tc)
        *content_state = svn_wc_notify_state_conflicted;
      else if (new_text_path)
        {
          if (is_locally_modified)
            *content_state = svn_wc_notify_state_merged;
          else
            *content_state = svn_wc_notify_state_changed;
        }
      else
        *content_state = svn_wc_notify_state_unchanged;
    }

  return SVN_NO_ERROR;
}



/* Mostly a wrapper around install_file. */
static svn_error_t *
close_file (void *file_baton,
            const char *text_checksum,
            apr_pool_t *pool)
{
  struct file_baton *fb = file_baton;
  const char *new_text_path = NULL, *parent_path;
  apr_array_header_t *propchanges = NULL;
  svn_wc_notify_state_t content_state, prop_state;
  svn_wc_adm_access_t *adm_access;

  /* window-handler assembles new pristine text in .svn/tmp/text-base/  */
  if (fb->text_changed)
    {
      new_text_path = svn_wc__text_base_path (fb->path, TRUE, pool);

      if (text_checksum)
        {
          const char *real_sum = svn_md5_digest_to_cstring (fb->digest, pool);
          
          if (real_sum && (strcmp (text_checksum, real_sum) != 0))
            return svn_error_createf
              (SVN_ERR_CHECKSUM_MISMATCH, NULL,
               "close_file: expected and actual checksums do not match:\n"
               "(%s):\n"
               "   expected checksum:  %s\n"
               "   actual checksum:    %s\n",
               fb->path, text_checksum, real_sum);
        }
    }

  if (fb->prop_changed)
    propchanges = fb->propchanges;

  parent_path = svn_path_dirname (fb->path, pool);
    
  SVN_ERR (svn_wc_adm_retrieve (&adm_access, fb->edit_baton->adm_access,
                                parent_path, pool));

  SVN_ERR (install_file (&content_state,
                         &prop_state,
                         adm_access,
                         fb->path,
                         fb->edit_baton->target_revision,
                         new_text_path,
                         propchanges,
                         FALSE, /* -not- a full proplist */
                         fb->new_URL,
                         FALSE,
                         NULL,
                         SVN_INVALID_REVNUM,
                         fb->edit_baton->diff3_cmd,
                         fb->last_changed_date,
                         pool));

  /* We have one less referrer to the directory's bump information. */
  SVN_ERR (maybe_bump_dir_info (fb->edit_baton,
                                fb->bump_info,
                                pool));

  if ((content_state != svn_wc_notify_state_unchanged) ||
      (prop_state != svn_wc_notify_state_unchanged))
    {
      if (fb->edit_baton->notify_func)
        (*fb->edit_baton->notify_func)
          (fb->edit_baton->notify_baton,
           fb->path,
           fb->added ? svn_wc_notify_update_add : svn_wc_notify_update_update,
           svn_node_file,
           NULL,  /* ### if install_file() gives mimetype, use it here */
           content_state,
           prop_state,
           SVN_INVALID_REVNUM);
    }
  return SVN_NO_ERROR;  
}


static svn_error_t *
close_edit (void *edit_baton,
            apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;
  const char *target_path = svn_path_join_many (pool, eb->anchor, 
                                                eb->target, NULL);

  /* The editor didn't even open the root; we have to take care of
     some cleanup stuffs. */
  if (! eb->root_opened)
    {
      /* If there is a target and that target is missing, then it
         apparently wasn't re-added by the update process (since the
         root directory wasn't even opened!), so we'll pretend that
         the editor actually did what it used to do (before a server
         bug was fixed): open_root, delete_entry, close_dir.  The
         helper function do_entry_deletion() will take care of the
         necessary steps.  */
      if ((eb->target) && (svn_wc__adm_missing (eb->adm_access, target_path)))
        SVN_ERR (do_entry_deletion (eb, eb->anchor, eb->target, pool));

      /* We need to "un-incomplete" the root directory. */
      SVN_ERR (complete_directory (eb, eb->anchor, TRUE, pool));
    }

  
  /* By definition, anybody "driving" this editor for update or switch
     purposes at a *minimum* must have called set_target_revision() at
     the outset, and close_edit() at the end -- even if it turned out
     that no changes ever had to be made, and open_root() was never
     called.  That's fine.  But regardless, when the edit is over,
     this editor needs to make sure that *all* paths have had their
     revisions bumped to the new target revision. */

  /* Make sure our update target now has the new working revision.
     Also, if this was an 'svn switch', then rewrite the target's
     url.  All of this tweaking might happen recursively!  Note
     that if eb->target is NULL, that's okay (albeit "sneaky",
     some might say).  */
  
  /* Extra check: if the update did nothing but make its target
     'deleted', then do *not* run cleanup on the target, as it
     will only remove the deleted entry!  */
  if (! eb->target_deleted)
    SVN_ERR (svn_wc__do_update_cleanup (target_path,
                                        eb->adm_access,
                                        eb->recurse,
                                        eb->switch_url,
                                        eb->target_revision,
                                        eb->notify_func,
                                        eb->notify_baton,
                                        TRUE,
                                        eb->pool));

  /* Let everyone know we're finished here. */
  if (eb->notify_func)
    (*eb->notify_func) (eb->notify_baton,
                        eb->anchor,
                        svn_wc_notify_update_completed,
                        svn_node_none,
                        NULL,
                        svn_wc_notify_state_inapplicable,
                        svn_wc_notify_state_inapplicable,
                        eb->target_revision);
  
  /* The edit is over, free its pool.
     ### No, this is wrong.  Who says this editor/baton won't be used
     again?  But the change is not merely to remove this call.  We
     should also make eb->pool not be a subpool (see make_editor),
     and change callers of svn_client_{checkout,update,switch} to do
     better pool management. ### */
  svn_pool_destroy (eb->pool);
  
  return SVN_NO_ERROR;
}



/*** Returning editors. ***/

/* Helper for the three public editor-supplying functions. */
static svn_error_t *
make_editor (svn_wc_adm_access_t *adm_access,
             const char *anchor,
             const char *target,
             svn_revnum_t target_revision,
             svn_boolean_t use_commit_times,
             const char *switch_url,
             svn_boolean_t recurse,
             svn_wc_notify_func_t notify_func,
             void *notify_baton,
             svn_cancel_func_t cancel_func,
             void *cancel_baton,
             const char *diff3_cmd,
             const svn_delta_editor_t **editor,
             void **edit_baton,
             svn_wc_traversal_info_t *traversal_info,
             apr_pool_t *pool)
{
  struct edit_baton *eb;
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_delta_editor_t *tree_editor = svn_delta_default_editor (subpool);

  /* Construct an edit baton. */
  eb = apr_pcalloc (subpool, sizeof (*eb));
  eb->pool            = subpool;
  eb->use_commit_times= use_commit_times;
  eb->target_revision = target_revision;
  eb->switch_url      = switch_url;
  eb->adm_access      = adm_access;
  eb->anchor          = anchor;
  eb->target          = target;
  eb->recurse         = recurse;
  eb->notify_func     = notify_func;
  eb->notify_baton    = notify_baton;
  eb->traversal_info  = traversal_info;
  eb->diff3_cmd       = diff3_cmd;
  eb->cancel_func     = cancel_func;
  eb->cancel_baton    = cancel_baton;

  /* Construct an editor. */
  tree_editor->set_target_revision = set_target_revision;
  tree_editor->open_root = open_root;
  tree_editor->delete_entry = delete_entry;
  tree_editor->add_directory = add_directory;
  tree_editor->open_directory = open_directory;
  tree_editor->change_dir_prop = change_dir_prop;
  tree_editor->close_directory = close_directory;
  tree_editor->add_file = add_file;
  tree_editor->open_file = open_file;
  tree_editor->apply_textdelta = apply_textdelta;
  tree_editor->change_file_prop = change_file_prop;
  tree_editor->close_file = close_file;
  tree_editor->close_edit = close_edit;

  SVN_ERR (svn_delta_get_cancellation_editor (cancel_func,
                                              cancel_baton,
                                              tree_editor,
                                              eb,
                                              editor,
                                              edit_baton,
                                              pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_get_update_editor (svn_wc_adm_access_t *anchor,
                          const char *target,
                          svn_revnum_t target_revision,
                          svn_boolean_t use_commit_times,
                          svn_boolean_t recurse,
                          svn_wc_notify_func_t notify_func,
                          void *notify_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          const char *diff3_cmd,
                          const svn_delta_editor_t **editor,
                          void **edit_baton,
                          svn_wc_traversal_info_t *traversal_info,
                          apr_pool_t *pool)
{
  return make_editor (anchor, svn_wc_adm_access_path (anchor),
                      target, target_revision, use_commit_times, NULL,
                      recurse, notify_func, notify_baton,
                      cancel_func, cancel_baton, diff3_cmd,
                      editor, edit_baton, traversal_info, pool);
}


svn_error_t *
svn_wc_get_switch_editor (svn_wc_adm_access_t *anchor,
                          const char *target,
                          svn_revnum_t target_revision,
                          const char *switch_url,
                          svn_boolean_t use_commit_times,
                          svn_boolean_t recurse,
                          svn_wc_notify_func_t notify_func,
                          void *notify_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          const char *diff3_cmd,
                          const svn_delta_editor_t **editor,
                          void **edit_baton,
                          svn_wc_traversal_info_t *traversal_info,
                          apr_pool_t *pool)
{
  assert (switch_url);

  return make_editor (anchor, svn_wc_adm_access_path (anchor),
                      target, target_revision, use_commit_times, switch_url,
                      recurse, notify_func, notify_baton,
                      cancel_func, cancel_baton, diff3_cmd,
                      editor, edit_baton,
                      traversal_info, pool);
}

svn_wc_traversal_info_t *
svn_wc_init_traversal_info (apr_pool_t *pool)
{
  svn_wc_traversal_info_t *ti = apr_palloc (pool, sizeof (*ti));
  
  ti->pool           = pool;
  ti->externals_old  = apr_hash_make (pool);
  ti->externals_new  = apr_hash_make (pool);
  
  return ti;
}


void
svn_wc_edited_externals (apr_hash_t **externals_old,
                         apr_hash_t **externals_new,
                         svn_wc_traversal_info_t *traversal_info)
{
  *externals_old = traversal_info->externals_old;
  *externals_new = traversal_info->externals_new;
}



/* THE GOAL

   Note the following actions, where X is the thing we wish to update,
   P is a directory whose repository URL is the parent of
   X's repository URL, N is directory whose repository URL is *not*
   the parent directory of X (including the case where N is not a
   versioned resource at all):

      1.  `svn up .' from inside X.
      2.  `svn up ...P/X' from anywhere.
      3.  `svn up ...N/X' from anywhere.

   For the purposes of the discussion, in the '...N/X' situation, X is
   said to be a "working copy (WC) root" directory.

   Now consider the four cases for X's type (file/dir) in the working
   copy vs. the repository:

      A.  dir in working copy, dir in repos.
      B.  dir in working copy, file in repos.
      C.  file in working copy, dir in repos.
      D.  file in working copy, file in repos.

   Here are the results we expect for each combination of the above:

      1A. Successfully update X.
      1B. Error (you don't want to remove your current working
          directory out from underneath the application).
      1C. N/A (you can't be "inside X" if X is a file).
      1D. N/A (you can't be "inside X" if X is a file).

      2A. Successfully update X.
      2B. Successfully update X.
      2C. Successfully update X.
      2D. Successfully update X.

      3A. Successfully update X.
      3B. Error (you can't create a versioned file X inside a
          non-versioned directory).
      3C. N/A (you can't have a versioned file X in directory that is
          not its repository parent).
      3D. N/A (you can't have a versioned file X in directory that is
          not its repository parent).

   To summarize, case 2 always succeeds, and cases 1 and 3 always fail
   (or can't occur) *except* when the target is a dir that remains a
   dir after the update.

   ACCOMPLISHING THE GOAL

   Updates are accomplished by driving an editor, and an editor is
   "rooted" on a directory.  So, in order to update a file, we need to
   break off the basename of the file, rooting the editor in that
   file's parent directory, and then updating only that file, not the
   other stuff in its parent directory.

   Secondly, we look at the case where we wish to update a directory.
   This is typically trivial.  However, one problematic case, exists
   when we wish to update a directory that has been removed from the
   repository and replaced with a file of the same name.  If we root
   our edit at the initial directory, there is no editor mechanism for
   deleting that directory and replacing it with a file (this would be
   like having an editor now anchored on a file, which is disallowed).

   All that remains is to have a function with the knowledge required
   to properly decide where to root our editor, and what to act upon
   with that now-rooted editor.  Given a path to be updated, this
   function should conditionally split that path into an "anchor" and
   a "target", where the "anchor" is the directory at which the update
   editor is rooted (meaning, editor->open_root() is called with
   this directory in mind), and the "target" is the actual intended
   subject of the update.

   svn_wc_get_actual_target() is that function.

   So, what are the conditions?

   Case I: Any time X is '.' (implying it is a directory), we won't
   lop off a basename.  So we'll root our editor at X, and update all
   of X.

   Cases II & III: Any time we are trying to update some path ...N/X,
   we again will not lop off a basename.  We can't root an editor at
   ...N with X as a target, either because ...N isn't a versioned
   resource at all (Case II) or because X is X is not a child of ...N
   in the repository (Case III).  We root at X, and update X.

   Cases IV-???: We lop off a basename when we are updating a
   path ...P/X, rooting our editor at ...P and updating X, or when X
   is missing from disk.

   These conditions apply whether X is a file or directory.

   ---

   As it turns out, commits need to have a similar check in place,
   too, specifically for the case where a single directory is being
   committed (we have to anchor at that directory's parent in case the
   directory itself needs to be modified) */
static svn_error_t *
check_wc_root (svn_boolean_t *wc_root,
               svn_node_kind_t *kind,
               const char *path, 
               svn_wc_adm_access_t *adm_access,
               apr_pool_t *pool)
{
  const char *parent, *base_name;
  const svn_wc_entry_t *p_entry, *entry;
  svn_error_t *err;

  /* Go ahead and initialize our return value to the most common
     (code-wise) values. */
  *wc_root = TRUE;

  /* Get our ancestry (this doubles as a sanity check).  */
  SVN_ERR (svn_wc_entry (&entry, path, adm_access, FALSE, pool));
  if (! entry)
    return svn_error_createf 
      (SVN_ERR_ENTRY_NOT_FOUND, NULL,
       "svn_wc_is_wc_root: '%s' is not a versioned resource", path);
  if (kind)
    *kind = entry->kind;

  /* If PATH is the current working directory, we have no choice but
     to consider it a WC root (we can't examine its parent at all) */
  if (svn_path_is_empty (path))
    return SVN_NO_ERROR;

  /* If we cannot get an entry for PATH's parent, PATH is a WC root. */
  p_entry = NULL;
  svn_path_split (path, &parent, &base_name, pool);
  err = svn_wc_adm_probe_open (&adm_access, NULL, parent, FALSE, FALSE,
                               pool);
  if (! err)
    err = svn_wc_entry (&p_entry, parent, adm_access, FALSE, pool);
  if (err || (! p_entry))
    {
      svn_error_clear (err);
      return SVN_NO_ERROR;
    }
  
  /* If the parent directory has no url information, something is
     messed up.  Bail with an error. */
  if (! p_entry->url)
    return svn_error_createf 
      (SVN_ERR_ENTRY_MISSING_URL, NULL,
       "svn_wc_is_wc_root: '%s' has no ancestry information.", 
       parent);

  /* If PATH's parent in the WC is not its parent in the repository,
     PATH is a WC root. */
  if (entry->url 
      && (strcmp (svn_path_url_add_component (p_entry->url, base_name, pool),
                  entry->url) != 0))
    return SVN_NO_ERROR;

  /* If we have not determined that PATH is a WC root by now, it must
     not be! */
  *wc_root = FALSE;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_is_wc_root (svn_boolean_t *wc_root,
                   const char *path,
                   svn_wc_adm_access_t *adm_access,
                   apr_pool_t *pool)
{
  return check_wc_root (wc_root, NULL, path, adm_access, pool);
}


svn_error_t *
svn_wc_get_actual_target (const char *path,
                          const char **anchor,
                          const char **target,
                          apr_pool_t *pool)
{
  svn_wc_adm_access_t *adm_access;
  svn_boolean_t is_wc_root;
  svn_node_kind_t kind;

  SVN_ERR (svn_wc_adm_probe_open (&adm_access, NULL, path, FALSE, FALSE, pool));
  SVN_ERR (check_wc_root (&is_wc_root, &kind, path, adm_access, pool));
  SVN_ERR (svn_wc_adm_close (adm_access));

  /* If PATH is not a WC root, or if it is a file, lop off a basename. */
  if ((! is_wc_root) || (kind == svn_node_file))
    {
      svn_path_split (path, anchor, target, pool);
      if ((*anchor)[0] == '\0')
        *anchor = "";
    }
  else
    {
      *anchor = apr_pstrdup (pool, path);
      *target = NULL;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_add_repos_file (const char *dst_path,
                       svn_wc_adm_access_t *adm_access,
                       const char *new_text_path,
                       apr_hash_t *new_props,
                       const char *copyfrom_url,
                       svn_revnum_t copyfrom_rev,
                       apr_pool_t *pool)
{
  const char *new_URL;
  apr_array_header_t *propchanges;

  /* Fabricate the anticipated new URL of the target. */
  {
    const svn_wc_entry_t *ent;
    const char *dir_name, *base_name;
    
    svn_path_split (dst_path, &dir_name, &base_name, pool);
    SVN_ERR (svn_wc_entry (&ent, dir_name, adm_access, FALSE, pool));
    new_URL = svn_path_url_add_component (ent->url, base_name, pool);
  }
  
  /* Construct the new properties.  Passing an empty hash for the
     source props will result in the right kind of prop array for
     install_file().  Ooh, magic. */
  SVN_ERR (svn_prop_diffs (&propchanges, new_props,
                           apr_hash_make (pool), pool));
  
  SVN_ERR (install_file (NULL,
                         NULL,
                         adm_access,
                         dst_path,
                         0,
                         new_text_path,
                         propchanges,
                         TRUE, /* a full proplist */
                         new_URL,
                         TRUE,
                         copyfrom_url,
                         copyfrom_rev,
                         NULL,
                         NULL,
                         pool));

  return SVN_NO_ERROR;
}
