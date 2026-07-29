/** @file
 *  @brief The project's C headers, opened for a C++ test.
 *
 *  Only client.h carries a linkage guard of its own; the rest of src/ is
 *  written for a C compiler and says nothing about C++.  Wrapping them here
 *  rather than in src/ keeps the units untouched and keeps the concession
 *  where it belongs — in the test that needs it.
 *
 *  Two things have to be bridged:
 *
 *  - **Linkage.** Without `extern "C"` every function would be looked up under
 *    a mangled name and no test would link.
 *  - **`_Noreturn`.** A C11 keyword that C++ never had.  It becomes the
 *    attribute that means the same thing, which stands in the same place.
 */

#pragma once

/* The headers the project pulls in from elsewhere carry linkage guards of their
   own, and some of them reach for C++ when a C++ compiler is reading — CRoaring
   asks for <atomic>, whose templates cannot stand inside a linkage block.  So
   they are taken first, in the open; by the time the project's own headers ask
   for them again, their include guards make it a no-op. */
#include <roaring/roaring.h>

#include "gradido_blockchain_core/memory.h"
#include "gradido_blockchain_core/result.h"
#include "gradido_blockchain_core/utils/bucket_vector.h"
#include "gradido_blockchain_core/utils/converter.h"
#include "gradido_blockchain_core/utils/mono_timer.h"

#ifdef __cplusplus
#ifndef _Noreturn
#define _Noreturn [[noreturn]]
#endif
extern "C" {
#endif

#include "client.h"
/* client.h names the kinds only in prose — a GeoAddress carries the number, not
   the enum — so the tests that check the numbering take it themselves. */
#include "types/geo_place_kind.h"

#include "doc_collector.h"
#include "error.h"
#include "format.h"
#include "geo_cell.h"
#include "geo_index.h"
#include "house_collector.h"
#include "json_parse.h"
#include "json_stats.h"
#include "line_buffer.h"
#include "meta_area_allocator.h"
#include "name_collector.h"
#include "parse_queue.h"
#include "place_cache.h"
#include "prefix_tree.h"
#include "progress.h"
#include "text_tokenize.h"

#ifdef __cplusplus
}
#endif
