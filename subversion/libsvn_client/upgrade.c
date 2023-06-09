/*
 * upgrade.c:  wrapper around wc upgrade functionality.
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

#include "svn_time.h"
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_config.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_props.h"
#include "svn_version.h"
#include "svn_hash.h"

#include "client.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"
#include "private/svn_subr_private.h"
#include "../libsvn_wc/wc.h"


/*** Code. ***/

/* callback baton for fetch_repos_info */
struct repos_info_baton
{
  apr_pool_t *state_pool;
  svn_client_ctx_t *ctx;
  const char *last_repos;
  const char *last_uuid;
};

/* svn_wc_upgrade_get_repos_info_t implementation for calling
   svn_wc_upgrade() from svn_client_upgrade() */
static svn_error_t *
fetch_repos_info(const char **repos_root,
                 const char **repos_uuid,
                 void *baton,
                 const char *url,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  struct repos_info_baton *ri = baton;

  /* The same info is likely to retrieved multiple times (e.g. externals) */
  if (ri->last_repos && svn_uri__is_ancestor(ri->last_repos, url))
    {
      *repos_root = apr_pstrdup(result_pool, ri->last_repos);
      *repos_uuid = apr_pstrdup(result_pool, ri->last_uuid);
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_client_get_repos_root(repos_root, repos_uuid, url, ri->ctx,
                                    result_pool, scratch_pool));

  /* Store data for further calls */
  ri->last_repos = apr_pstrdup(ri->state_pool, *repos_root);
  ri->last_uuid = apr_pstrdup(ri->state_pool, *repos_uuid);

  return SVN_NO_ERROR;
}

/* Forward definition. Upgrades svn:externals properties in the working copy
   LOCAL_ABSPATH to the WC-NG  storage. INFO_BATON will be used to fetch
   repository info using fetch_repos_info() function if needed.
 */
static svn_error_t *
upgrade_externals_from_properties(svn_client_ctx_t *ctx,
                                  const char *local_abspath,
                                  int wc_format,
                                  struct repos_info_baton *info_baton,
                                  apr_pool_t *scratch_pool);

static svn_error_t *
upgrade_internal(int *result_format_p,
                 const char *path,
                 int target_format,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *scratch_pool)
{
  const char *local_abspath;
  apr_hash_t *externals;
  struct repos_info_baton info_baton;

  info_baton.state_pool = scratch_pool;
  info_baton.ctx = ctx;
  info_baton.last_repos = NULL;
  info_baton.last_uuid = NULL;

  if (svn_path_is_url(path))
    return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                             _("'%s' is not a local path"), path);

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, scratch_pool));
  SVN_ERR(svn_wc__upgrade(result_format_p, ctx->wc_ctx,
                          local_abspath, target_format,
                          fetch_repos_info, &info_baton,
                          ctx->cancel_func, ctx->cancel_baton,
                          ctx->notify_func2, ctx->notify_baton2,
                          scratch_pool));

  SVN_ERR(svn_wc__externals_defined_below(&externals,
                                          ctx->wc_ctx, local_abspath,
                                          scratch_pool, scratch_pool));

  if (apr_hash_count(externals) > 0)
    {
      apr_pool_t *iterpool = svn_pool_create(scratch_pool);
      apr_hash_index_t *hi;

      /* We are upgrading from >= 1.7. No need to upgrade from
         svn:externals properties. And by that avoiding the removal
         of recorded externals information (issue #4519)

         Only directory externals need an explicit upgrade */
      for (hi = apr_hash_first(scratch_pool, externals);
           hi;
           hi = apr_hash_next(hi))
        {
          const char *ext_abspath;
          svn_node_kind_t kind;

          svn_pool_clear(iterpool);

          ext_abspath = apr_hash_this_key(hi);

          SVN_ERR(svn_wc__read_external_info(&kind, NULL, NULL, NULL, NULL,
                                             ctx->wc_ctx, local_abspath,
                                             ext_abspath, FALSE,
                                             iterpool, iterpool));

          if (kind == svn_node_dir)
            {
              svn_error_t *err = upgrade_internal(NULL, ext_abspath,
                                                  target_format, ctx,
                                                  iterpool);

              if (err)
                {
                  svn_wc_notify_t *notify =
                            svn_wc_create_notify(ext_abspath,
                                                 svn_wc_notify_failed_external,
                                                 iterpool);
                  notify->err = err;
                  ctx->notify_func2(ctx->notify_baton2,
                                    notify, iterpool);
                  svn_error_clear(err);
                  /* Next external node, please... */
                }
            }
        }

      svn_pool_destroy(iterpool);
    }
  else
    {
      /* Upgrading from <= 1.6, or no svn:properties defined.
         (There is no way to detect the difference from libsvn_client :( ) */

      SVN_ERR(upgrade_externals_from_properties(ctx, local_abspath,
                                                target_format, &info_baton,
                                                scratch_pool));
    }

  SVN_ERR(svn_client__textbase_sync(NULL, local_abspath, FALSE, TRUE, ctx,
                                    NULL, scratch_pool, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_upgrade2(const svn_version_t **result_format_version_p,
                    const char *path,
                    const svn_version_t *target_format_version,
                    svn_client_ctx_t *ctx,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  int result_format;
  int target_format;
  svn_boolean_t fail_on_downgrade;

  if (target_format_version)
    {
      SVN_ERR(svn_wc__format_from_version(&target_format,
                                          target_format_version,
                                          scratch_pool));
      /* Fail on downgrade attempts if format version was passed explicitly. */
      fail_on_downgrade = TRUE;
    }
  else
    {
      const svn_version_t *default_version;

      SVN_ERR(svn_client_default_wc_version(&default_version, ctx,
                                            scratch_pool, scratch_pool));
      SVN_ERR(svn_wc__format_from_version(&target_format, default_version,
                                          scratch_pool));
      fail_on_downgrade = FALSE;
    }

  SVN_ERR(upgrade_internal(&result_format, path, target_format,
                           ctx, scratch_pool));

  if (fail_on_downgrade && result_format > target_format)
    return svn_error_createf(SVN_ERR_WC_UNSUPPORTED_FORMAT, NULL,
                             _("Working copy '%s' is already at version %s "
                               "(format %d) and cannot be downgraded to "
                               "version %s (format %d)"),
                             svn_dirent_local_style(path, scratch_pool),
                             svn_wc__version_string_from_format(result_format),
                             result_format,
                             svn_wc__version_string_from_format(target_format),
                             target_format);

  if (result_format_version_p)
    *result_format_version_p = svn_client_wc_version_from_format(
                                 result_format, result_pool);

  return SVN_NO_ERROR;
}

const svn_version_t *
svn_client_wc_version_from_format(int wc_format,
                                  apr_pool_t *result_pool)
{
  static const svn_version_t
    version_1_0  = { 1, 0, 0, NULL },
    version_1_4  = { 1, 4, 0, NULL },
    version_1_5  = { 1, 5, 0, NULL },
    version_1_6  = { 1, 6, 0, NULL },
    version_1_7  = { 1, 7, 0, NULL },
    version_1_8  = { 1, 8, 0, NULL },
    version_1_15 = { 1, 15, 0, NULL };

  switch (wc_format)
    {
      case  4: return &version_1_0;
      case  8: return &version_1_4;
      case  9: return &version_1_5;
      case 10: return &version_1_6;
      case 29: return &version_1_7;
      case 31: return &version_1_8;
      case 32: return &version_1_15;
    }
  return NULL;
}

const int *
svn_client_get_wc_formats_supported(apr_pool_t *result_pool)
{
  static const int versions[] = {
    SVN_WC__SUPPORTED_VERSION,
    SVN_WC__VERSION,
    0
  };

  return versions;
}

const svn_version_t *
svn_client_oldest_wc_version(apr_pool_t *result_pool)
{
  /* NOTE: For consistency, always return the version of the client
     that first introduced the format. */
  static const svn_version_t version = { 1, 8, 0, NULL };
  return &version;
}

svn_error_t *
svn_client_default_wc_version(const svn_version_t **version_p,
                              svn_client_ctx_t *ctx,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  svn_config_t *config;
  const char *value;
  svn_version_t *version;

  if (ctx->config)
    config = svn_hash_gets(ctx->config, SVN_CONFIG_CATEGORY_CONFIG);
  else
    config = NULL;

  svn_config_get(config, &value,
                 SVN_CONFIG_SECTION_WORKING_COPY,
                 SVN_CONFIG_OPTION_COMPATIBLE_VERSION,
                 NULL);
  if (value)
    {
      SVN_ERR(svn_version__parse_version_string(&version, value, result_pool));
    }
  else
    {
      /* NOTE: For consistency, always return the version of the client
         that first introduced the format. */
      version = apr_pcalloc(result_pool, sizeof(*version));
      version->major = 1;
      version->minor = 8;
      version->patch = 0;
      version->tag = NULL;
    }

  *version_p = version;
  return SVN_NO_ERROR;
}

const svn_version_t *
svn_client_latest_wc_version(apr_pool_t *result_pool)
{
  /* NOTE: For consistency, always return the version of the client
     that first introduced the format. */
  static const svn_version_t version = { 1, 15, 0, NULL };
  return &version;
}

const svn_version_t *
svn_client__compatible_wc_version_optional_pristine(apr_pool_t *result_pool)
{
  /* NOTE: For consistency, always return the version of the client
     that first introduced the format. */
  static const svn_version_t version = { 1, 15, 0, NULL };
  return &version;
}

/* Helper for upgrade_externals_from_properties: upgrades one external ITEM
   in EXTERNALS_PARENT. Uses SCRATCH_POOL for temporary allocations. */
static svn_error_t *
upgrade_external_item(svn_client_ctx_t *ctx,
                      int wc_format,
                      const char *externals_parent_abspath,
                      const char *externals_parent_url,
                      const char *externals_parent_repos_root_url,
                      svn_wc_external_item2_t *item,
                      struct repos_info_baton *info_baton,
                      apr_pool_t *scratch_pool)
{
  const char *resolved_url;
  const char *external_abspath;
  const char *repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_node_kind_t external_kind;
  svn_revnum_t peg_revision;
  svn_revnum_t revision;
  svn_error_t *err;

  external_abspath = svn_dirent_join(externals_parent_abspath,
                                     item->target_dir,
                                     scratch_pool);

  SVN_ERR(svn_wc__resolve_relative_external_url(
              &resolved_url,
              item,
              externals_parent_repos_root_url,
              externals_parent_url,
              scratch_pool, scratch_pool));

  /* This is a hack. We only need to call svn_wc__upgrade() on external
   * dirs, as file externals are upgraded along with their defining
   * WC.  Reading the kind will throw an exception on an external dir,
   * saying that the wc must be upgraded.  If it's a file, the lookup
   * is done in an adm_dir belonging to the defining wc (which has
   * already been upgraded) and no error is returned.  If it doesn't
   * exist (external that isn't checked out yet), we'll just get
   * svn_node_none. */
  err = svn_wc_read_kind2(&external_kind, ctx->wc_ctx,
                          external_abspath, TRUE, FALSE, scratch_pool);
  if (err && err->apr_err == SVN_ERR_WC_UPGRADE_REQUIRED)
    {
      svn_error_clear(err);

      SVN_ERR(upgrade_internal(NULL, external_abspath, wc_format, ctx,
                               scratch_pool));
    }
  else if (err)
    return svn_error_trace(err);

  /* The upgrade of any dir should be done now, get the now reliable
   * kind. */
  SVN_ERR(svn_wc_read_kind2(&external_kind, ctx->wc_ctx, external_abspath,
                            TRUE, FALSE, scratch_pool));

  /* Update the EXTERNALS table according to the root URL,
   * relpath and uuid known in the upgraded external WC. */

  /* We should probably have a function that provides all three
   * of root URL, repos relpath and uuid at once, but here goes... */

  /* First get the relpath, as that returns SVN_ERR_WC_PATH_NOT_FOUND
   * when the node is not present in the file system.
   * svn_wc__node_get_repos_info() would try to derive the URL. */
  SVN_ERR(svn_wc__node_get_repos_info(NULL,
                                      &repos_relpath,
                                      &repos_root_url,
                                      &repos_uuid,
                                      ctx->wc_ctx,
                                      external_abspath,
                                      scratch_pool, scratch_pool));

  /* If we haven't got any information from the checked out external,
   * or if the URL information mismatches the external's definition,
   * ask fetch_repos_info() to find out the repos root. */
  if (0 != strcmp(resolved_url,
                  svn_path_url_add_component2(repos_root_url,
                                              repos_relpath,
                                              scratch_pool)))
    {
      SVN_ERR(fetch_repos_info(&repos_root_url, &repos_uuid, info_baton,
                               resolved_url, scratch_pool, scratch_pool));

      repos_relpath = svn_uri_skip_ancestor(repos_root_url,
                                            resolved_url,
                                            scratch_pool);

      /* There's just the URL, no idea what kind the external is.
       * That's fine, as the external isn't even checked out yet.
       * The kind will be set during the next 'update'. */
      external_kind = svn_node_unknown;
    }

  peg_revision = (item->peg_revision.kind == svn_opt_revision_number
                     ? item->peg_revision.value.number
                     : SVN_INVALID_REVNUM);

  revision = (item->revision.kind == svn_opt_revision_number
                 ? item->revision.value.number
                 : SVN_INVALID_REVNUM);

  SVN_ERR(svn_wc__upgrade_add_external_info(ctx->wc_ctx,
                                            external_abspath,
                                            external_kind,
                                            externals_parent_abspath,
                                            repos_relpath,
                                            repos_root_url,
                                            repos_uuid,
                                            peg_revision,
                                            revision,
                                            scratch_pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
upgrade_externals_from_properties(svn_client_ctx_t *ctx,
                                  const char *local_abspath,
                                  int wc_format,
                                  struct repos_info_baton *info_baton,
                                  apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;
  apr_pool_t *iterpool;
  apr_pool_t *inner_iterpool;
  apr_hash_t *externals;
  svn_opt_revision_t rev = {svn_opt_revision_unspecified, {0}};

  /* Now it's time to upgrade the externals too. We do it after the wc
     upgrade to avoid that errors in the externals causes the wc upgrade to
     fail. Thanks to caching the performance penalty of walking the wc a
     second time shouldn't be too severe */
  SVN_ERR(svn_client_propget5(&externals, NULL, SVN_PROP_EXTERNALS,
                              local_abspath, &rev, &rev, NULL,
                              svn_depth_infinity, NULL, ctx,
                              scratch_pool, scratch_pool));

  iterpool = svn_pool_create(scratch_pool);
  inner_iterpool = svn_pool_create(scratch_pool);

  for (hi = apr_hash_first(scratch_pool, externals); hi;
       hi = apr_hash_next(hi))
    {
      int i;
      const char *externals_parent_url;
      const char *externals_parent_repos_root_url;
      const char *externals_parent_repos_relpath;
      const char *externals_parent_abspath = apr_hash_this_key(hi);
      svn_string_t *external_desc = apr_hash_this_val(hi);
      apr_array_header_t *externals_p;
      svn_error_t *err;

      svn_pool_clear(iterpool);

      /* svn_client_propget5() has API promise to return absolute paths. */
      SVN_ERR_ASSERT(svn_dirent_is_absolute(externals_parent_abspath));

      externals_p = apr_array_make(iterpool, 1,
                                   sizeof(svn_wc_external_item2_t*));

      /* In this loop, an error causes the respective externals definition, or
       * the external (inner loop), to be skipped, so that upgrade carries on
       * with the other externals. */
      err = svn_wc__node_get_repos_info(NULL,
                                        &externals_parent_repos_relpath,
                                        &externals_parent_repos_root_url,
                                        NULL,
                                        ctx->wc_ctx,
                                        externals_parent_abspath,
                                        iterpool, iterpool);

      if (!err)
        {
          err = svn_wc_parse_externals_description3(
              &externals_p, svn_dirent_dirname(local_abspath, iterpool),
              external_desc->data, FALSE, iterpool);
        }

      if (err)
        {
          svn_wc_notify_t *notify =
              svn_wc_create_notify(externals_parent_abspath,
                                   svn_wc_notify_failed_external,
                                   scratch_pool);
          notify->err = err;

          ctx->notify_func2(ctx->notify_baton2,
                            notify, scratch_pool);

          svn_error_clear(err);

          /* Next externals definition, please... */
          continue;
        }

      externals_parent_url = svn_path_url_add_component2(
          externals_parent_repos_root_url,
          externals_parent_repos_relpath,
          iterpool);

      for (i = 0; i < externals_p->nelts; i++)
        {
          svn_wc_external_item2_t *item;

          item = APR_ARRAY_IDX(externals_p, i, svn_wc_external_item2_t*);

          svn_pool_clear(inner_iterpool);
          err = upgrade_external_item(ctx, wc_format,
                                      externals_parent_abspath,
                                      externals_parent_url,
                                      externals_parent_repos_root_url,
                                      item, info_baton, inner_iterpool);

          if (err)
            {
              svn_wc_notify_t *notify =
                  svn_wc_create_notify(svn_dirent_join(externals_parent_abspath,
                                                       item->target_dir,
                                                       inner_iterpool),
                                       svn_wc_notify_failed_external,
                                       scratch_pool);
              notify->err = err;
              ctx->notify_func2(ctx->notify_baton2,
                                notify, scratch_pool);
              svn_error_clear(err);
              /* Next external node, please... */
            }
        }
    }

  svn_pool_destroy(inner_iterpool);
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}
