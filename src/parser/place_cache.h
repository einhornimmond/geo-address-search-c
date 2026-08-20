/** @defgroup place_cache Place cache
 *  @ingroup parser
 *  @brief The dump, written down once in the shape the later walks need — so
 *         that two of the three passes never touch the compressed file again.
 *
 *  A build walks the dump three times: once for the vocabulary, once for the
 *  documents, once for the house numbers.  Measured, all three cost almost
 *  exactly what unpacking the file costs and nothing more — 680 GB of JSON
 *  come out of 24 GB of zstd at 105 MB/s, and eight parser threads finish
 *  faster than the single stream can feed them.  The dump is one zstd frame,
 *  so that stream cannot be shared out either.
 *
 *  So the first pass writes what it reads: every entry, stripped to the fields
 *  the later passes name, in a fixed binary form.  The second and third pass
 *  read that instead — a tenth of the bytes, no decompression, no JSON.
 *
 *  ### Two files, because the passes want different halves
 *
 *  A house number is payload of its street and never becomes a document; a
 *  street never carries a number.  Written apart, each pass reads only what it
 *  will use — the documents file holds 18 % of the entries, the houses file
 *  the rest, and neither pass walks past the other's records.
 *
 *  ### One pair per thread
 *
 *  A thread writes what it parsed and reads it back in the passes that follow.
 *  Nothing is shared, nothing is locked, and the per-thread collectors merge
 *  afterwards exactly as they do without the cache.
 *
 *  ### And the next build starts here
 *
 *  A sealed cache outlives the run that wrote it.  Where the dump has not
 *  changed and the layout has not moved — see @ref PlaceCacheStamp — even the
 *  first pass reads it instead of the dump, and a rebuild never unpacks a byte.
 *  That is what the manifest is for: it is written last, names what the cache
 *  was made from, and a cache without one is not read.
 *
 *  ### What is not kept
 *
 *  Only what @c collect_document and @c collect_house read.  A house record
 *  carries four texts and a point; a document record adds its own name, its
 *  weight, its kind and the search texts.  Everything else the dump offers —
 *  the thirty translated names of every ancestor, the object ids, the
 *  categories — went into the vocabulary in the first pass and is never asked
 *  for again.
 *
 *  The narrowness is the point and the risk both.  Should a later pass come to
 *  read a field the cache does not carry, it would find nothing and say
 *  nothing.  @ref PLACE_CACHE_LAYOUT guards the file format; a field added to
 *  a record has to raise it, or a stale file will be read as if it were whole.
 *
 *  @whisper What was unpacked once is set down in the shape it will be needed
 *  @{
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hostmem/result.h"
#include "parser/json_parse.h"

/** Eight bytes opening every cache file. */
#define PLACE_CACHE_MAGIC "GRDPCACH"

/** Raise this when a record gains, loses or reorders a field. */
#define PLACE_CACHE_LAYOUT 1u

/** Most parser threads a build may use, and so most files a cache may hold.
 *  Removal reaches this far whatever this run was asked for — otherwise a cache
 *  written by more threads leaves its remainder behind for good. */
#define PLACE_CACHE_THREADS_MAX 10u

/** Which half of the dump a file holds. */
typedef enum PlaceCacheKind {
  PLACE_CACHE_DOCUMENTS, /**< Entries that become a document of their own. */
  PLACE_CACHE_HOUSES     /**< Entries that hang a number on a street. */
} PlaceCacheKind;

/** Writing end of one thread's pair of files. */
typedef struct PlaceCacheWriter {
  FILE *file[2];     /**< One per @ref PlaceCacheKind. */
  char *path[2];     /**< Where they lie, for the removal at the end. */
  uint64_t count[2]; /**< Records written. */
  uint64_t bytes[2]; /**< Bytes written, header included. */
  char *buffer;      /**< One record under construction. */
  size_t capacity;   /**< Room in @c buffer; grows to the longest record seen. */
} PlaceCacheWriter;

/** Reading end of one file. */
typedef struct PlaceCacheReader {
  FILE *file;
  PlaceCacheKind kind;
  char *buffer;    /**< The record just read; the strings point into it. */
  size_t capacity; /**< Room in @c buffer. */
  uint64_t count;  /**< Records read so far. */
  uint64_t bytes;  /**< Bytes read so far, header included. */
  /** Set when the walk stopped at something other than the end of the file.
   *  A caller that ignores it builds an index out of half a cache. */
  bool broken;
} PlaceCacheReader;

/**
 * @brief What binds a cache to one dump and one build.
 *
 *  A cache is worth reading again only when nothing it was made from has
 *  moved: the same dump, byte for byte and to the second; the same number of
 *  threads, because a thread reads back what it wrote; and the same record
 *  layout, because a field added to a record is a field an older file does not
 *  carry and would answer with silence.
 */
typedef struct PlaceCacheStamp {
  uint64_t dump_bytes; /**< Size of the compressed dump. */
  uint64_t dump_mtime; /**< Its last modification, in seconds. */
  uint32_t threads;    /**< Parser threads the files were written by. */
  uint32_t layout;     /**< @ref PLACE_CACHE_LAYOUT at the time of writing. */
} PlaceCacheStamp;

/**
 * @brief Take the stamp a cache for this dump would have to carry.
 *
 *  @param[in]  dump_path  The compressed dump.
 *  @param[in]  threads    Parser threads this run will use.
 *  @param[out] out        Receives the stamp.
 *  @return false when the dump cannot be looked at.
 */
bool place_cache_stamp_of(const char *dump_path, unsigned threads, PlaceCacheStamp *out);

/**
 * @brief Does a cache under @p directory answer for exactly this run?
 *
 *  Reads the manifest, compares every field of the stamp, and looks that each
 *  file it promises is really there — a run killed between writing the files
 *  and sealing them, or after sealing and before finishing, must not be taken
 *  for a whole one.
 *
 *  @param[in] directory  Where a cache might lie.
 *  @param[in] want       Stamp from place_cache_stamp_of().
 *  @return true when the cache may be read instead of the dump.
 *
 *  @whisper What was set down once is recognised, or quietly set aside
 */
bool place_cache_is_current(const char *directory, const PlaceCacheStamp *want);

/**
 * @brief Seal a freshly written cache with its manifest.
 *
 *  Called only once every file is closed: the manifest is what makes the cache
 *  readable again, so it is written last and removed if it cannot be written
 *  whole.
 *
 *  @param[in] directory  Where the files lie.
 *  @param[in] stamp      Stamp from place_cache_stamp_of().
 *  @return HOSTMEM_SUCCESS, HOSTMEM_ERROR_NULL_POINTER, HOSTMEM_ERROR_OUT_OF_MEMORY or
 *          HOSTMEM_ERROR_ENCODE_FAILED.
 */
hostmem_result place_cache_seal(const char *directory, const PlaceCacheStamp *stamp);

/**
 * @brief Remove the manifest and every file of a cache.
 *
 *  Reaches at least @ref PLACE_CACHE_THREADS_MAX files wide, whatever @p threads
 *  says, so that a cache written by a wider run leaves nothing behind.  Missing
 *  files are no error.
 */
void place_cache_discard(const char *directory, unsigned threads);

/**
 * @brief Bytes the files of one kind hold together, for a progress line.
 *
 *  @param[in] directory  Where the cache lies.
 *  @param[in] threads    Files to count, one per thread.
 *  @param[in] kind       Which half.
 *  @return The sum; 0 when nothing of that kind is there.
 */
uint64_t place_cache_bytes(const char *directory, unsigned threads, PlaceCacheKind kind);

/** Why a directory can or cannot hold a cache. */
typedef enum PlaceCacheRoom {
  PLACE_CACHE_ROOM_OK,            /**< It is there, it is writable, and it is big enough. */
  PLACE_CACHE_ROOM_UNMAKEABLE,    /**< It is not there and could not be made. */
  PLACE_CACHE_ROOM_NOT_DIRECTORY, /**< Something of that name is there and is no directory. */
  PLACE_CACHE_ROOM_UNWRITABLE,    /**< A directory nothing may be written into. */
  PLACE_CACHE_ROOM_TOO_SMALL      /**< Room, but not enough of it. */
} PlaceCacheRoom;

/**
 * @brief Make room under @p directory for a cache of this dump, or say why not.
 *
 *  The directory is created when it is not there — a path named for a cache is
 *  a path meant to hold one, and asking the caller to make it first only turns
 *  a spelling mistake into a silent fallback.  Missing parents are made too.
 *
 *  The cache is written uncompressed and comes to roughly the size of the
 *  compressed dump — measured on a planet dump, 25 GB against 26 GB.  The
 *  demand is set at twice that, which leaves the same room again for the index
 *  being written beside it and for a dump that compressed better than most.
 *
 *  @param[in]  directory   Where the cache should lie; made if it is not there.
 *  @param[in]  dump_bytes  Size of the compressed dump.
 *  @param[out] out_free    Receives the free bytes found; 0 where the
 *                          directory could not be reached.  May be NULL.
 *  @return PLACE_CACHE_ROOM_OK, or what stands in the way.
 *
 *  @whisper A place is made ready, or the reason it cannot be is spoken plainly
 */
PlaceCacheRoom place_cache_make_room(
    const char *directory, uint64_t dump_bytes, uint64_t *out_free
);

/**
 * @brief Free bytes a cache for a dump of this size is asked to have.
 *
 *  Measured: a planet cache comes to 119 % of the packed dump, and the demand
 *  sits at 150 % of it — enough room for the cache and a quarter of the dump
 *  again, and still a refusal where a disk would fill up halfway through.
 *
 *  @param[in] dump_bytes  Size of the compressed dump.
 *  @return The bytes @ref place_cache_make_room will insist on.
 */
uint64_t place_cache_wanted(uint64_t dump_bytes);

/** @brief What @ref place_cache_make_room found, as a sentence for a reader. */
const char *place_cache_room_reason(PlaceCacheRoom room);

/**
 * @brief Open one thread's pair of files for writing.
 *
 *  Any file of an earlier run under the same name is replaced.
 *
 *  @param[out] writer     Receives the open pair; zeroed on failure.
 *  @param[in]  directory  Where the files are laid down.
 *  @param[in]  thread     Thread number, part of the file name.
 *  @return HOSTMEM_SUCCESS, HOSTMEM_ERROR_NULL_POINTER, HOSTMEM_ERROR_OUT_OF_MEMORY, or
 *          HOSTMEM_ERROR_ENCODE_FAILED when a file could not be opened.
 */
hostmem_result place_cache_writer_open(
    PlaceCacheWriter *writer, const char *directory, unsigned thread
);

/**
 * @brief Write one entry into the file its kind belongs to.
 *
 *  An entry that becomes neither a document nor a house — a house-level record
 *  the dump gave no number — is passed over silently; nothing will ask for it.
 *
 *  @param[in,out] writer  Open writer.
 *  @param[in]     place   Entry as the parser handed it over.
 *  @return HOSTMEM_SUCCESS, HOSTMEM_ERROR_NULL_POINTER, or HOSTMEM_ERROR_ENCODE_FAILED.
 */
hostmem_result place_cache_write(PlaceCacheWriter *writer, const PhotonPlace *place);

/** @brief Flush and close both files, leaving them on disk. Safe with NULL. */
void place_cache_writer_close(PlaceCacheWriter *writer);

/** @brief Close what is still open and delete both files. Safe with NULL. */
void place_cache_writer_remove(PlaceCacheWriter *writer);

/**
 * @brief Open one file for reading.
 *
 *  Checks magic, layout and kind before a single record is handed out.
 *
 *  @param[out] reader     Receives the open file; zeroed on failure.
 *  @param[in]  directory  Where the files were laid down.
 *  @param[in]  thread     Thread number the file was written under.
 *  @param[in]  kind       Which half to read.
 *  @return HOSTMEM_SUCCESS, HOSTMEM_ERROR_NULL_POINTER, HOSTMEM_ERROR_OUT_OF_MEMORY,
 *          HOSTMEM_ERROR_DECODE_FAILED when the file cannot be opened, or
 *          HOSTMEM_ERROR_INVALID_PARAM when it was not written by this build.
 */
hostmem_result place_cache_reader_open(
    PlaceCacheReader *reader, const char *directory, unsigned thread, PlaceCacheKind kind
);

/**
 * @brief Read the next entry.
 *
 *  The strings of @p out point into the reader's own buffer and stay valid
 *  until the next call — the same promise the JSON parser makes, so a callback
 *  written for one works unchanged for the other.
 *
 *  @param[in,out] reader  Open reader.
 *  @param[out]    out     Receives the entry, zeroed first.
 *  @return true while a record was read; false at the end of the file.
 *
 *  @whisper What was set down comes back in the shape it was needed in
 */
bool place_cache_read(PlaceCacheReader *reader, PhotonPlace *out);

/** @brief Close the file. Safe to call with NULL. */
void place_cache_reader_close(PlaceCacheReader *reader);

/** @} */
