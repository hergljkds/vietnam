/*
 * checkout.c:  wrappers around wc checkout functionality
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include "svn_pools.h"
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_ra.h"
#include "svn_types.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_io.h"
#include "svn_opt.h"
#include "svn_time.h"
#include "svn_version.h"
#include "client.h"

#include "private/svn_subr_private.h"
#include "private/svn_wc_private.h"

#include "svn_private_config.h"


/*** Public Interfaces. ***/

static svn_error_t *
initialize_area(int target_format,
                const char *local_abspath,
                const svn_client__pathrev_t *pathrev,
                svn_depth_t depth,
                svn_boolean_t store_pristine,
                svn_client_ctx_t *ctx,
                apr_pool_t *pool)
{
  if (depth == svn_depth_unknown)
    depth = svn_depth_infinity;

  /* Make the unversioned directory into a versioned one.  */
  SVN_ERR(svn_wc__ensure_adm(ctx->wc_ctx,
                             target_format, local_abspath, pathrev->url,
                             pathrev->repos_root_url, pathrev->repos_uuid,
                             pathrev->rev, depth, store_pristine, pool));
  return SVN_NO_ERROR;
}


svn_error_t *
svn_client__checkout_internal(svn_revnum_t *result_rev,
                              svn_boolean_t *timestamp_sleep,
                              const char *url,
                              const char *local_abspath,
                              const svn_opt_revision_t *peg_revision,
                              const svn_opt_revision_t *revision,
                              svn_depth_t depth,
                              svn_boolean_t ignore_externals,
                              svn_boolean_t allow_unver_obstructions,
                              svn_boolean_t settings_from_context,
                              const svn_version_t *wc_format_version,
                              svn_tristate_t store_pristine,
                              svn_ra_session_t *ra_session,
                              svn_client_ctx_t *ctx,
                              apr_pool_t *scratch_pool)
{
  int target_format;
  svn_boolean_t target_store_pristine;
  svn_boolean_t fail_on_format_mismatch;
  svn_node_kind_t kind;
  svn_client__pathrev_t *pathrev;
  svn_opt_revision_t resolved_rev = { svn_opt_revision_number };

  /* Sanity check.  Without these, the checkout is meaningless. */
  SVN_ERR_ASSERT(local_abspath != NULL);
  SVN_ERR_ASSERT(svn_uri_is_canonical(url, scratch_pool));
  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  /* Fulfill the docstring promise of svn_client_checkout: */
  if ((revision->kind != svn_opt_revision_number)
      && (revision->kind != svn_opt_revision_date)
      && (revision->kind != svn_opt_revision_head))
    return svn_error_create(SVN_ERR_CLIENT_BAD_REVISION, NULL, NULL);

  if (settings_from_context)
    {
      SVN_ERR(svn_wc__settings_from_context(&target_format,
                                            &target_store_pristine,
                                            ctx->wc_ctx, local_abspath,
                                            scratch_pool));
      fail_on_format_mismatch = FALSE;
    }
  else
    {
      const svn_version_t *target_format_version;

      if (store_pristine == svn_tristate_unknown)
        target_store_pristine = TRUE;
      else if (store_pristine == svn_tristate_true)
        target_store_pristine = TRUE;
      else
        target_store_pristine = FALSE;

      if (wc_format_version)
        {
          target_format_version = wc_format_version;
          /* Fail if the existing WC's format is different than requested. */
          fail_on_format_mismatch = TRUE;
        }
      else
        {
          /* A NULL wc_format_version translates to the minimum compatible
             version. */
          SVN_ERR(svn_client_default_wc_version(&target_format_version, ctx,
                                                scratch_pool, scratch_pool));

          if (!target_store_pristine)
            {
              const svn_version_t *required_version =
                svn_client__compatible_wc_version_optional_pristine(scratch_pool);

              if (!svn_version__at_least(target_format_version,
                                         required_version->major,
                                         required_version->minor,
                                         required_version->patch))
                target_format_version = required_version;
            }

          fail_on_format_mismatch = FALSE;
        }

      SVN_ERR(svn_wc__format_from_version(&target_format,
                                          target_format_version,
                                          scratch_pool));
    }

  /* Get the RA connection, if needed. */
  if (ra_session)
    {
      svn_error_t *err = svn_ra_reparent(ra_session, url, scratch_pool);

      if (err)
        {
          if (err->apr_err == SVN_ERR_RA_ILLEGAL_URL)
            {
              svn_error_clear(err);
              ra_session = NULL;
            }
          else
            return svn_error_trace(err);
        }
      else
        {
          SVN_ERR(svn_client__resolve_rev_and_url(&pathrev,
                                                  ra_session, url,
                                                  peg_revision, revision,
                                                  ctx, scratch_pool));
        }
    }

  if (!ra_session)
    {
      SVN_ERR(svn_client__ra_session_from_path2(&ra_session, &pathrev,
                                                url, NULL, peg_revision,
                                                revision, ctx, scratch_pool));
    }

  SVN_ERR(svn_ra_check_path(ra_session, "", pathrev->rev, &kind, scratch_pool));
  resolved_rev.value.number = pathrev->rev;

  if (kind == svn_node_none)
    return svn_error_createf(SVN_ERR_RA_ILLEGAL_URL, NULL,
                             _("URL '%s' doesn't exist"), pathrev->url);
  else if (kind == svn_node_file)
    return svn_error_createf
      (SVN_ERR_UNSUPPORTED_FEATURE , NULL,
       _("URL '%s' refers to a file, not a directory"), pathrev->url);

  SVN_ERR(svn_io_check_path(local_abspath, &kind, scratch_pool));

  if (kind == svn_node_none)
    {
      /* Bootstrap: create an incomplete working-copy root dir.  Its
         entries file should only have an entry for THIS_DIR with a
         URL, revnum, and an 'incomplete' flag.  */
      SVN_ERR(svn_io_make_dir_recursively(local_abspath, scratch_pool));
      SVN_ERR(initialize_area(target_format, local_abspath, pathrev, depth,
                              target_store_pristine, ctx, scratch_pool));
    }
  else if (kind == svn_node_dir)
    {
      int present_format;
      const char *entry_url;

      SVN_ERR(svn_wc_check_wc2(&present_format, ctx->wc_ctx, local_abspath,
                               scratch_pool));

      if (! present_format)
        {
          SVN_ERR(initialize_area(target_format, local_abspath, pathrev, depth,
                                  target_store_pristine, ctx, scratch_pool));
        }
      else
        {
          svn_boolean_t wc_store_pristine;

          SVN_ERR(svn_wc__get_settings(NULL, &wc_store_pristine, ctx->wc_ctx,
                                       local_abspath, scratch_pool));

          if ((target_store_pristine && !wc_store_pristine) ||
              (!target_store_pristine && wc_store_pristine))
            {
              return svn_error_createf(
                  SVN_ERR_WC_INCOMPATIBLE_SETTINGS, NULL,
                  _("'%s' is an existing working copy with different '%s' setting"),
                  svn_dirent_local_style(local_abspath, scratch_pool),
                  "store-pristine");
            }

          /* Get PATH's URL. */
          SVN_ERR(svn_wc__node_get_url(&entry_url, ctx->wc_ctx, local_abspath,
                                       scratch_pool, scratch_pool));

          /* If PATH's existing URL matches the incoming one, then
             just update.  This allows 'svn co' to restart an
             interrupted checkout.  Otherwise bail out. */
          if (strcmp(entry_url, pathrev->url) != 0)
            return svn_error_createf(
                SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                _("'%s' is already a working copy for a different URL"),
                svn_dirent_local_style(local_abspath, scratch_pool));

          if (fail_on_format_mismatch && present_format != target_format)
            return svn_error_createf(
                SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                _("'%s' is already a working copy for the same URL"
                  " but its format is %d instead of the expected %d"),
                svn_dirent_local_style(local_abspath, scratch_pool),
                present_format, target_format);
        }
    }
  else
    {
      return svn_error_createf(SVN_ERR_WC_NODE_KIND_CHANGE, NULL,
                               _("'%s' already exists and is not a directory"),
                               svn_dirent_local_style(local_abspath,
                                                      scratch_pool));
    }

  /* Have update fix the incompleteness. */
  SVN_ERR(svn_client__update_internal(result_rev, timestamp_sleep,
                                      local_abspath, &resolved_rev, depth,
                                      TRUE, ignore_externals,
                                      allow_unver_obstructions,
                                      TRUE /* adds_as_modification */,
                                      FALSE, FALSE, ra_session,
                                      ctx, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_checkout4(svn_revnum_t *result_rev,
                     const char *URL,
                     const char *path,
                     const svn_opt_revision_t *peg_revision,
                     const svn_opt_revision_t *revision,
                     svn_depth_t depth,
                     svn_boolean_t ignore_externals,
                     svn_boolean_t allow_unver_obstructions,
                     const svn_version_t *wc_format_version,
                     svn_tristate_t store_pristine,
                     svn_client_ctx_t *ctx,
                     apr_pool_t *pool)
{
  const char *local_abspath;
  svn_error_t *err;
  svn_boolean_t sleep_here = FALSE;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

  err = svn_client__checkout_internal(result_rev, &sleep_here,
                                      URL, local_abspath,
                                      peg_revision, revision, depth,
                                      ignore_externals,
                                      allow_unver_obstructions,
                                      FALSE, /* settings_from_context */
                                      wc_format_version,
                                      store_pristine,
                                      NULL /* ra_session */,
                                      ctx, pool);
  if (sleep_here)
    svn_io_sleep_for_timestamps(local_abspath, pool);

  return svn_error_trace(err);
}
