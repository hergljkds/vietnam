/*
 * wc_db_pristine.c :  Pristine ("text base") management
 *
 * See the spec in 'notes/wc-ng/pristine-store'.
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

#define SVN_WC__I_AM_WC_DB

#include "svn_pools.h"
#include "svn_io.h"
#include "svn_dirent_uri.h"

#include "private/svn_io_private.h"

#include "wc.h"
#include "wc_db.h"
#include "wc-queries.h"
#include "wc_db_private.h"

#define PRISTINE_STORAGE_EXT ".svn-base"
#define PRISTINE_STORAGE_RELPATH "pristine"
#define PRISTINE_TEMPDIR_RELPATH "tmp"



/* Returns in PRISTINE_ABSPATH a new string allocated from RESULT_POOL,
   holding the local absolute path to the file location that is dedicated
   to hold CHECKSUM's pristine file, relating to the pristine store
   configured for the working copy indicated by PDH. The returned path
   does not necessarily currently exist.

   Any other allocations are made in SCRATCH_POOL. */
static svn_error_t *
get_pristine_fname(const char **pristine_abspath,
                   const char *wcroot_abspath,
                   const svn_checksum_t *sha1_checksum,
                   apr_pool_t *result_pool,
                   apr_pool_t *scratch_pool)
{
  const char *base_dir_abspath;
  const char *hexdigest = svn_checksum_to_cstring(sha1_checksum, scratch_pool);
  char subdir[3];

  /* ### code is in transition. make sure we have the proper data.  */
  SVN_ERR_ASSERT(pristine_abspath != NULL);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(wcroot_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  base_dir_abspath = svn_dirent_join_many(scratch_pool,
                                          wcroot_abspath,
                                          svn_wc_get_adm_dir(scratch_pool),
                                          PRISTINE_STORAGE_RELPATH,
                                          SVN_VA_NULL);

  /* We should have a valid checksum and (thus) a valid digest. */
  SVN_ERR_ASSERT(hexdigest != NULL);

  /* Get the first two characters of the digest, for the subdir. */
  subdir[0] = hexdigest[0];
  subdir[1] = hexdigest[1];
  subdir[2] = '\0';

  hexdigest = apr_pstrcat(scratch_pool, hexdigest, PRISTINE_STORAGE_EXT,
                          SVN_VA_NULL);

  /* The file is located at DIR/.svn/pristine/XX/XXYYZZ...svn-base */
  *pristine_abspath = svn_dirent_join_many(result_pool,
                                           base_dir_abspath,
                                           subdir,
                                           hexdigest,
                                           SVN_VA_NULL);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_get_future_path(const char **pristine_abspath,
                                    const char *wcroot_abspath,
                                    const svn_checksum_t *sha1_checksum,
                                    apr_pool_t *result_pool,
                                    apr_pool_t *scratch_pool)
{
  SVN_ERR(get_pristine_fname(pristine_abspath, wcroot_abspath,
                             sha1_checksum,
                             result_pool, scratch_pool));
  return SVN_NO_ERROR;
}

/* Read *HAVE_ROW, *MD5_CHECKSUM, *SIZE, *HYDRATED for WCROOT:SHA1_CHECKSUM.
 * If it returns with *HAVE_ROW=FALSE, other outputs are null/zero/false.
 */
static svn_error_t *
stmt_select_pristine(svn_boolean_t *have_row,
                     const svn_checksum_t **md5_checksum,
                     svn_filesize_t *size,
                     svn_boolean_t *hydrated,
                     svn_wc__db_wcroot_t *wcroot,
                     const svn_checksum_t *sha1_checksum,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  int stmt_num;
  svn_sqlite__stmt_t *stmt;

  /* Check that this pristine text is present in the store.  (The presence
   * of the file is not sufficient.) */
  stmt_num = (wcroot->format >= SVN_WC__HAS_OPTIONAL_PRISTINE
              ? STMT_SELECT_PRISTINE_F32
              : STMT_SELECT_PRISTINE_F31);
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, stmt_num));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__step(have_row, stmt));
  if (!*have_row)
    {
      if (md5_checksum)
        *md5_checksum = NULL;
      if (size)
        *size = 0;
      if (hydrated)
        *hydrated = FALSE;
      return svn_sqlite__reset(stmt);
    }

  if (md5_checksum)
    SVN_ERR(svn_sqlite__column_checksum(md5_checksum, stmt, 0, result_pool));
  if (size)
    *size = svn_sqlite__column_int64(stmt, 1);
  if (hydrated)
    *hydrated = svn_sqlite__column_boolean(stmt, 2);

  SVN_ERR(svn_sqlite__reset(stmt));
  return SVN_NO_ERROR;
}

/* Set *CONTENTS to a readable stream from which the pristine text
 * identified by SHA1_CHECKSUM and PRISTINE_ABSPATH can be read from the
 * pristine store of WCROOT.  If the pristine contents are currently not
 * available on disk, set *CONTENTS to NULL.  If SIZE is not null, set
 * *SIZE to the size in bytes of that text.  If that text is not in
 * the pristine store, return an error.
 *
 * Even if the pristine text is removed from the store while it is being
 * read, the stream will remain valid and readable until it is closed.
 *
 * Allocate the stream in RESULT_POOL.
 *
 * This function expects to be executed inside a SQLite txn.
 *
 * Implements 'notes/wc-ng/pristine-store' section A-3(d).
 */
static svn_error_t *
pristine_read_txn(svn_stream_t **contents,
                  svn_filesize_t *size,
                  svn_wc__db_wcroot_t *wcroot,
                  const svn_checksum_t *sha1_checksum,
                  const char *pristine_abspath,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  svn_boolean_t have_row;
  svn_boolean_t hydrated;

  /* Check that this pristine text is present in the store.  (The presence
   * of the file is not sufficient.) */
  SVN_ERR(stmt_select_pristine(&have_row, NULL, size, &hydrated,
                               wcroot, sha1_checksum,
                               result_pool, scratch_pool));
  if (! have_row)
    {
      return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                               _("Pristine text '%s' not present"),
                               svn_checksum_to_cstring_display(
                                 sha1_checksum, scratch_pool));
    }

  if (contents)
    {
      if (hydrated)
        {
          /* Open the file as a readable stream.  It will remain readable even when
           * deleted from disk; APR guarantees that on Windows as well as Unix.
           *
           * We also don't enable APR_BUFFERED on this file to maximize throughput
           * e.g. for fulltext comparison.  As we use SVN__STREAM_CHUNK_SIZE buffers
           * where needed in streams, there is no point in having another layer of
           * buffers. */

          apr_file_t *file;
          SVN_ERR(svn_io_file_open(&file, pristine_abspath, APR_READ,
                                   APR_OS_DEFAULT, result_pool));
          *contents = svn_stream_from_aprfile2(file, FALSE, result_pool);
        }
      else
        {
          *contents = NULL;
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pristine_read(svn_stream_t **contents,
                         svn_filesize_t *size,
                         svn_wc__db_t *db,
                         const char *wri_abspath,
                         const svn_checksum_t *sha1_checksum,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  const char *pristine_abspath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));

  /* Some 1.6-to-1.7 wc upgrades created rows without checksums and
     updating such a row passes NULL here. */
  if (!sha1_checksum)
    return svn_error_createf(SVN_ERR_WC_CORRUPT, NULL,
                             _("Can't read '%s' from pristine store "
                               "because no checksum supplied"),
                             svn_dirent_local_style(wri_abspath, scratch_pool));

  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(get_pristine_fname(&pristine_abspath, wcroot->abspath,
                             sha1_checksum,
                             scratch_pool, scratch_pool));
  SVN_WC__DB_WITH_TXN(
    pristine_read_txn(contents, size,
                      wcroot, sha1_checksum, pristine_abspath,
                      result_pool, scratch_pool),
    wcroot);

  return SVN_NO_ERROR;
}


struct svn_wc__db_install_data_t
{
  svn_wc__db_wcroot_t *wcroot;
  svn_stream_t *inner_stream;
  apr_off_t size;
};

static svn_error_t *
install_stream_write_fn(void *baton, const char *data, apr_size_t *len)
{
  svn_wc__db_install_data_t *install_data = baton;

  if (install_data->inner_stream)
    SVN_ERR(svn_stream_write(install_data->inner_stream, data, len));

  install_data->size += *len;

  return SVN_NO_ERROR;
}

static svn_error_t *
install_stream_seek_fn(void *baton, const svn_stream_mark_t *mark)
{
  svn_wc__db_install_data_t *install_data = baton;

  if (!mark)
    return svn_error_create(SVN_ERR_STREAM_SEEK_NOT_SUPPORTED, NULL, NULL);

  if (install_data->inner_stream)
    SVN_ERR(svn_stream_reset(install_data->inner_stream));

  install_data->size = 0;

  return SVN_NO_ERROR;
}

static svn_error_t *
install_stream_close_fn(void *baton)
{
  svn_wc__db_install_data_t *install_data = baton;

  if (install_data->inner_stream)
    SVN_ERR(svn_stream_close(install_data->inner_stream));

  return SVN_NO_ERROR;
}

/* Return the absolute path to the temporary directory for pristine text
   files within WCROOT. */
static char *
pristine_get_tempdir(svn_wc__db_wcroot_t *wcroot,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  return svn_dirent_join_many(result_pool, wcroot->abspath,
                              svn_wc_get_adm_dir(scratch_pool),
                              PRISTINE_TEMPDIR_RELPATH, SVN_VA_NULL);
}

/* Install the pristine text described by INSTALL_DATA into the pristine store
 * of SDB.
 *
 * This function expects to be executed inside a SQLite txn that has already
 * acquired a 'RESERVED' lock.
 *
 * Implements 'notes/wc-ng/pristine-store' section A-3(a).
 */
static svn_error_t *
pristine_install_txn(svn_wc__db_wcroot_t *wcroot,
                     svn_wc__db_install_data_t *install_data,
                     /* The target path for the file (within the pristine store). */
                     const char *pristine_abspath,
                     /* The pristine text's SHA-1 checksum. */
                     const svn_checksum_t *sha1_checksum,
                     /* The pristine text's MD-5 checksum. */
                     const svn_checksum_t *md5_checksum,
                     apr_pool_t *scratch_pool)
{
  svn_stream_t *install_stream = install_data->inner_stream;
  int stmt_num;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  svn_boolean_t hydrated;

  SVN_ERR(stmt_select_pristine(&have_row, NULL, NULL, &hydrated,
                               wcroot, sha1_checksum,
                               scratch_pool, scratch_pool));

  if (have_row && hydrated)
    {
      /* For now, ensure that we do not inadvertently dehydrate an existing
       * hydrated entry, as there could be references to its content. */

      if (install_stream)
        SVN_ERR(svn_stream__install_delete(install_stream, scratch_pool));

      return SVN_NO_ERROR;
    }

  if (install_stream)
    {
      /* Move the file to its target location.  (If it is already there, it is
       * an orphan file and it doesn't matter if we overwrite it.) */

      svn_stream__install_set_read_only(install_stream, TRUE);

      SVN_ERR(svn_stream__install_finalize(NULL, NULL, install_stream,
                                           scratch_pool));
      SVN_ERR(svn_stream__install_stream(install_stream, pristine_abspath,
                                         TRUE, scratch_pool));
    }
  else
    {
      SVN_ERR(svn_io_remove_file2(pristine_abspath, TRUE, scratch_pool));
    }

  stmt_num = (wcroot->format >= SVN_WC__HAS_OPTIONAL_PRISTINE
              ? STMT_UPSERT_PRISTINE_F32 : STMT_UPSERT_PRISTINE_F31);
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, stmt_num));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 2, md5_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__bind_int64(stmt, 3, install_data->size));
  if (wcroot->format >= SVN_WC__HAS_OPTIONAL_PRISTINE)
    SVN_ERR(svn_sqlite__bind_int(stmt, 4, install_stream != NULL));
  SVN_ERR(svn_sqlite__insert(NULL, stmt));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pristine_prepare_install(svn_stream_t **stream_p,
                                    svn_wc__db_install_data_t **install_data_p,
                                    svn_checksum_t **sha1_checksum_p,
                                    svn_checksum_t **md5_checksum_p,
                                    svn_wc__db_t *db,
                                    const char *wri_abspath,
                                    svn_boolean_t hydrated,
                                    apr_pool_t *result_pool,
                                    apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_wc__db_pristine_prepare_install_internal(
            stream_p, install_data_p, sha1_checksum_p, md5_checksum_p,
            wcroot, hydrated, result_pool, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pristine_prepare_install_internal(svn_stream_t **stream_p,
                                             svn_wc__db_install_data_t **install_data_p,
                                             svn_checksum_t **sha1_checksum_p,
                                             svn_checksum_t **md5_checksum_p,
                                             svn_wc__db_wcroot_t *wcroot,
                                             svn_boolean_t hydrated,
                                             apr_pool_t *result_pool,
                                             apr_pool_t *scratch_pool)
{
  svn_stream_t *stream;
  svn_wc__db_install_data_t *install_data;
  const char *temp_dir_abspath;

  temp_dir_abspath = pristine_get_tempdir(wcroot, scratch_pool, scratch_pool);

  install_data = apr_pcalloc(result_pool, sizeof(*install_data));
  install_data->wcroot = wcroot;

  if (hydrated || wcroot->store_pristine)
    {
      SVN_ERR_W(svn_stream__create_for_install(&install_data->inner_stream,
                                               temp_dir_abspath,
                                               result_pool, scratch_pool),
                _("Unable to create pristine install stream"));
    }
  else
    {
      install_data->inner_stream = NULL;
    }

  install_data->size = 0;

  stream = svn_stream_create(install_data, result_pool);
  svn_stream_set_write(stream, install_stream_write_fn);
  svn_stream_set_seek(stream, install_stream_seek_fn);
  svn_stream_set_close(stream, install_stream_close_fn);

  if (md5_checksum_p)
    stream = svn_stream_checksummed2(stream, NULL, md5_checksum_p,
                                     svn_checksum_md5, FALSE, result_pool);
  if (sha1_checksum_p)
    stream = svn_stream_checksummed2(stream, NULL, sha1_checksum_p,
                                     svn_checksum_sha1, FALSE, result_pool);

  *stream_p = stream;
  *install_data_p = install_data;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pristine_install(svn_wc__db_install_data_t *install_data,
                            const svn_checksum_t *sha1_checksum,
                            const svn_checksum_t *md5_checksum,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot = install_data->wcroot;
  const char *pristine_abspath;

  SVN_ERR_ASSERT(sha1_checksum != NULL);
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);
  SVN_ERR_ASSERT(md5_checksum != NULL);
  SVN_ERR_ASSERT(md5_checksum->kind == svn_checksum_md5);

  SVN_ERR(get_pristine_fname(&pristine_abspath, wcroot->abspath,
                             sha1_checksum,
                             scratch_pool, scratch_pool));

  /* Ensure the SQL txn has at least a 'RESERVED' lock before we start looking
   * at the disk, to ensure no concurrent pristine install/delete txn. */
  SVN_SQLITE__WITH_IMMEDIATE_TXN(
    pristine_install_txn(wcroot,
                         install_data, pristine_abspath,
                         sha1_checksum, md5_checksum,
                         scratch_pool),
    wcroot->sdb);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pristine_install_abort(svn_wc__db_install_data_t *install_data,
                                  apr_pool_t *scratch_pool)
{
  if (install_data->inner_stream)
    SVN_ERR(svn_stream__install_delete(install_data->inner_stream, scratch_pool));

  install_data->size = 0;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_get_md5(const svn_checksum_t **md5_checksum,
                            svn_wc__db_t *db,
                            const char *wri_abspath,
                            const svn_checksum_t *sha1_checksum,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(stmt_select_pristine(&have_row, md5_checksum, NULL, NULL,
                               wcroot, sha1_checksum,
                               result_pool, scratch_pool));
  if (!have_row)
    return svn_error_createf(SVN_ERR_WC_DB_ERROR, NULL,
                             _("The pristine text with checksum '%s' was "
                               "not found"),
                             svn_checksum_to_cstring_display(sha1_checksum,
                                                             scratch_pool));

  SVN_ERR_ASSERT((*md5_checksum)->kind == svn_checksum_md5);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_get_sha1(const svn_checksum_t **sha1_checksum,
                             svn_wc__db_t *db,
                             const char *wri_abspath,
                             const svn_checksum_t *md5_checksum,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  SVN_ERR_ASSERT(md5_checksum->kind == svn_checksum_md5);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_PRISTINE_BY_MD5));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, md5_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    return svn_error_createf(SVN_ERR_WC_DB_ERROR, svn_sqlite__reset(stmt),
                             _("The pristine text with MD5 checksum '%s' was "
                               "not found"),
                             svn_checksum_to_cstring_display(md5_checksum,
                                                             scratch_pool));

  SVN_ERR(svn_sqlite__column_checksum(sha1_checksum, stmt, 0, result_pool));
  SVN_ERR_ASSERT((*sha1_checksum)->kind == svn_checksum_sha1);

  return svn_error_trace(svn_sqlite__reset(stmt));
}

/* If the pristine text referenced by SHA1_CHECKSUM in WCROOT/SDB, whose path
 * within the pristine store is PRISTINE_ABSPATH, has a reference count of
 * zero, delete it (both the database row and the disk file).
 *
 * This function expects to be executed inside a SQLite txn that has already
 * acquired a 'RESERVED' lock.
 */
static svn_error_t *
pristine_remove_if_unreferenced_txn(svn_sqlite__db_t *sdb,
                                    svn_wc__db_wcroot_t *wcroot,
                                    const svn_checksum_t *sha1_checksum,
                                    const char *pristine_abspath,
                                    apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  int affected_rows;

  /* Remove the DB row, if refcount is 0. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                    STMT_DELETE_PRISTINE_IF_UNREFERENCED));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__update(&affected_rows, stmt));

  /* If we removed the DB row, then remove the file. */
  if (affected_rows > 0)
    SVN_ERR(svn_io_remove_file2(pristine_abspath, TRUE, scratch_pool));

  return SVN_NO_ERROR;
}

/* If the pristine text referenced by SHA1_CHECKSUM in WCROOT has a
 * reference count of zero, delete it (both the database row and the disk
 * file).
 *
 * Implements 'notes/wc-ng/pristine-store' section A-3(b). */
static svn_error_t *
pristine_remove_if_unreferenced(svn_wc__db_wcroot_t *wcroot,
                                const svn_checksum_t *sha1_checksum,
                                apr_pool_t *scratch_pool)
{
  const char *pristine_abspath;

  SVN_ERR(get_pristine_fname(&pristine_abspath, wcroot->abspath,
                             sha1_checksum, scratch_pool, scratch_pool));

  /* Ensure the SQL txn has at least a 'RESERVED' lock before we start looking
   * at the disk, to ensure no concurrent pristine install/delete txn. */
  SVN_SQLITE__WITH_IMMEDIATE_TXN(
    pristine_remove_if_unreferenced_txn(
      wcroot->sdb, wcroot, sha1_checksum, pristine_abspath, scratch_pool),
    wcroot->sdb);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pristine_remove(svn_wc__db_t *db,
                           const char *wri_abspath,
                           const svn_checksum_t *sha1_checksum,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  /* ### Transitional: accept MD-5 and look up the SHA-1.  Return an error
   * if the pristine text is not in the store. */
  if (sha1_checksum->kind != svn_checksum_sha1)
    SVN_ERR(svn_wc__db_pristine_get_sha1(&sha1_checksum, db, wri_abspath,
                                         sha1_checksum,
                                         scratch_pool, scratch_pool));
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* If the work queue is not empty, don't delete any pristine text because
   * the work queue may contain a reference to it. */
  {
    svn_sqlite__stmt_t *stmt;
    svn_boolean_t have_row;

    SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_LOOK_FOR_WORK));
    SVN_ERR(svn_sqlite__step(&have_row, stmt));
    SVN_ERR(svn_sqlite__reset(stmt));

    if (have_row)
      return SVN_NO_ERROR;
  }

  /* If not referenced, remove the PRISTINE table row and the file. */
  SVN_ERR(pristine_remove_if_unreferenced(wcroot, sha1_checksum, scratch_pool));

  return SVN_NO_ERROR;
}


/* Remove all unreferenced pristines in the WC DB in WCROOT.
 *
 * Look for pristine texts whose 'refcount' in the DB is zero, and remove
 * them from the 'pristine' table and from disk.
 *
 * TODO: At least check that any zero refcount is really correct, before
 *       using it.  See dev@ email thread "Pristine text missing - cleanup
 *       doesn't work", <http://svn.haxx.se/dev/archive-2013-04/0426.shtml>.
 *
 * TODO: Ideas for possible extra clean-up operations:
 *
 *       * Check and correct all the refcounts.  Identify any rows missing
 *         from the 'pristine' table.  (Create a temporary index for speed
 *         if necessary?)
 *
 *       * Check the checksums.  (Very expensive to check them all, so find
 *         a way to not check them all.)
 *
 *       * Check for pristine files missing from disk but referenced in the
 *         'pristine' table.
 *
 *       * Repair any pristine files missing from disk and/or rows missing
 *         from the 'pristine' table and/or bad checksums.  Generally
 *         requires contacting the server, so requires support at a higher
 *         level than this function.
 *
 *       * Identify any pristine text files on disk that are not referenced
 *         in the DB, and delete them.
 *
 * TODO: Provide feedback about any errors found and any corrections made.
 */
static svn_error_t *
pristine_cleanup_wcroot(svn_wc__db_wcroot_t *wcroot,
                        apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_error_t *err = NULL;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  /* Find each unreferenced pristine in the DB and remove it. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_UNREFERENCED_PRISTINES));
  while (! err)
    {
      svn_boolean_t have_row;
      const svn_checksum_t *sha1_checksum;

      svn_pool_clear(iterpool);

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
      if (! have_row)
        break;

      SVN_ERR(svn_sqlite__column_checksum(&sha1_checksum, stmt, 0,
                                          iterpool));
      err = pristine_remove_if_unreferenced(wcroot, sha1_checksum,
                                            iterpool);
    }

  svn_pool_destroy(iterpool);

  return svn_error_trace(
      svn_error_compose_create(err, svn_sqlite__reset(stmt)));
}

svn_error_t *
svn_wc__db_pristine_cleanup(svn_wc__db_t *db,
                            const char *wri_abspath,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(pristine_cleanup_wcroot(wcroot, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_check(svn_boolean_t *present,
                          svn_boolean_t *hydrated,
                          svn_wc__db_t *db,
                          const char *wri_abspath,
                          const svn_checksum_t *sha1_checksum,
                          apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);

  if (sha1_checksum->kind != svn_checksum_sha1)
    {
      *present = FALSE;
      if (hydrated)
        *hydrated = FALSE;

      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* Check that there is an entry in the PRISTINE table. */
  SVN_ERR(stmt_select_pristine(&have_row, NULL, NULL, hydrated,
                               wcroot, sha1_checksum,
                               scratch_pool, scratch_pool));
  *present = have_row;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_dehydrate(svn_wc__db_t *db,
                              const char *wri_abspath,
                              const svn_checksum_t *sha1_checksum,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_wc__db_pristine_dehydrate_internal(wcroot, sha1_checksum,
                                                 scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_dehydrate_internal(svn_wc__db_wcroot_t *wcroot,
                                       const svn_checksum_t *sha1_checksum,
                                       apr_pool_t *scratch_pool)
{
  const char *pristine_abspath;
  svn_sqlite__stmt_t *stmt;

  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  SVN_ERR(get_pristine_fname(&pristine_abspath, wcroot->abspath,
                             sha1_checksum,
                             scratch_pool, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_UPDATE_PRISTINE_HYDRATED));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__bind_int(stmt, 2, FALSE));
  SVN_ERR(svn_sqlite__update(NULL, stmt));

  SVN_ERR(svn_io_remove_file2(pristine_abspath, TRUE, scratch_pool));

  return SVN_NO_ERROR;
}
