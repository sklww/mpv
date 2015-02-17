/*
 * This file is part of MPlayer.
 *
 * Original author: Uoti Urpala
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "talloc.h"

#include "player/core.h"
#include "common/msg.h"
#include "demux/demux.h"
#include "options/path.h"
#include "misc/bstr.h"
#include "common/common.h"
#include "stream/stream.h"

#define HEADER "# mpv EDL v0\n"

struct tl_part {
    char *filename;             // what is stream_open()ed
    double offset;              // offset into the source file
    bool offset_set;
    bool chapter_ts;
    double length;              // length of the part (-1 if rest of the file)
};

struct tl_parts {
    struct tl_part *parts;
    int num_parts;
};

// Parse a time (absolute file time or duration). Currently equivalent to a
// number. Return false on failure.
static bool parse_time(bstr str, double *out_time)
{
    bstr rest;
    double time = bstrtod(str, &rest);
    if (!str.len || rest.len || !isfinite(time))
        return false;
    *out_time = time;
    return true;
}

/* Returns a list of parts, or NULL on parse error.
 * Syntax (without file header or URI prefix):
 *    url      ::= <entry> ( (';' | '\n') <entry> )*
 *    entry    ::= <param> ( <param> ',' )*
 *    param    ::= [<string> '='] (<string> | '%' <number> '%' <bytes>)
 */
static struct tl_parts *parse_edl(bstr str)
{
    struct tl_parts *tl = talloc_zero(NULL, struct tl_parts);
    while (str.len) {
        if (bstr_eatstart0(&str, "#"))
            bstr_split_tok(str, "\n", &(bstr){0}, &str);
        if (bstr_eatstart0(&str, "\n") || bstr_eatstart0(&str, ";"))
            continue;
        struct tl_part p = { .length = -1 };
        int nparam = 0;
        while (1) {
            bstr name, val;
            // Check if it's of the form "name=..."
            int next = bstrcspn(str, "=%,;\n");
            if (next > 0 && next < str.len && str.start[next] == '=') {
                name = bstr_splice(str, 0, next);
                str = bstr_cut(str, next + 1);
            } else {
                const char *names[] = {"file", "start", "length"}; // implied name
                name = bstr0(nparam < 3 ? names[nparam] : "-");
            }
            if (bstr_eatstart0(&str, "%")) {
                int len = bstrtoll(str, &str, 0);
                if (!bstr_startswith0(str, "%") || (len > str.len - 1))
                    goto error;
                val = bstr_splice(str, 1, len + 1);
                str = bstr_cut(str, len + 1);
            } else {
                next = bstrcspn(str, ",;\n");
                val = bstr_splice(str, 0, next);
                str = bstr_cut(str, next);
            }
            // Interpret parameters. Explicitly ignore unknown ones.
            if (bstr_equals0(name, "file")) {
                p.filename = bstrto0(tl, val);
            } else if (bstr_equals0(name, "start")) {
                if (!parse_time(val, &p.offset))
                    goto error;
                p.offset_set = true;
            } else if (bstr_equals0(name, "length")) {
                if (!parse_time(val, &p.length))
                    goto error;
            } else if (bstr_equals0(name, "timestamps")) {
                if (bstr_equals0(val, "chapters"))
                    p.chapter_ts = true;
            }
            nparam++;
            if (!bstr_eatstart0(&str, ","))
                break;
        }
        if (!p.filename)
            goto error;
        MP_TARRAY_APPEND(tl, tl->parts, tl->num_parts, p);
    }
    if (!tl->num_parts)
        goto error;
    return tl;
error:
    talloc_free(tl);
    return NULL;
}

static struct demuxer *open_file(char *filename, struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct demuxer *d = NULL;
    struct stream *s = stream_open(filename, mpctx->global);
    if (s) {
        stream_enable_cache(&s, &opts->stream_cache);
        d = demux_open(s, NULL, NULL, mpctx->global);
    }
    if (!d) {
        MP_ERR(mpctx, "EDL: Could not open source file '%s'.\n",
               filename);
        free_stream(s);
    }
    return d;
}

static struct demuxer *open_source(struct MPContext *mpctx, char *filename)
{
    for (int n = 0; n < mpctx->num_sources; n++) {
        struct demuxer *d = mpctx->sources[n];
        if (strcmp(d->stream->url, filename) == 0)
            return d;
    }
    struct demuxer *d = open_file(filename, mpctx);
    if (d)
        MP_TARRAY_APPEND(NULL, mpctx->sources, mpctx->num_sources, d);
    return d;
}

static double demuxer_chapter_time(struct demuxer *demuxer, int n)
{
    if (n < 0 || n >= demuxer->num_chapters)
        return -1;
    return demuxer->chapters[n].pts;
}

// Append all chapters from src to the chapters array.
// Ignore chapters outside of the given time range.
static void copy_chapters(struct demux_chapter **chapters, int *num_chapters,
                          struct demuxer *src, double start, double len,
                          double dest_offset)
{
    for (int n = 0; n < src->num_chapters; n++) {
        double time = demuxer_chapter_time(src, n);
        if (time >= start && time <= start + len) {
            struct demux_chapter ch = {
                .pts = dest_offset + time - start,
                .name = talloc_strdup(*chapters, src->chapters[n].name),
            };
            MP_TARRAY_APPEND(NULL, *chapters, *num_chapters, ch);
        }
    }
}

// return length of the source in seconds, or -1 if unknown
static double source_get_length(struct demuxer *demuxer)
{
    double time;
    // <= 0 means DEMUXER_CTRL_NOTIMPL or DEMUXER_CTRL_DONTKNOW
    if (demux_control(demuxer, DEMUXER_CTRL_GET_TIME_LENGTH, &time) <= 0)
        time = -1;
    return time;
}

static void resolve_timestamps(struct tl_part *part, struct demuxer *demuxer)
{
    if (part->chapter_ts) {
        double start = demuxer_chapter_time(demuxer, part->offset);
        double length = part->length;
        double end = length;
        if (end >= 0)
            end = demuxer_chapter_time(demuxer, part->offset + part->length);
        if (end >= 0 && start >= 0)
            length = end - start;
        part->offset = start;
        part->length = length;
    }
    if (!part->offset_set)
        part->offset = demuxer->start_time;
}

static void build_timeline(struct MPContext *mpctx, struct tl_parts *parts)
{
    struct demux_chapter *chapters = talloc_new(NULL);
    int num_chapters = 0;
    struct timeline_part *timeline = talloc_array_ptrtype(NULL, timeline,
                                                          parts->num_parts + 1);
    double starttime = 0;
    for (int n = 0; n < parts->num_parts; n++) {
        struct tl_part *part = &parts->parts[n];
        struct demuxer *source = open_source(mpctx, part->filename);
        if (!source)
            goto error;

        resolve_timestamps(part, source);

        double len = source_get_length(source);
        if (len > 0) {
            len += source->start_time;
        } else {
            MP_WARN(mpctx, "EDL: source file '%s' has unknown duration.\n",
                   part->filename);
        }

        // Unknown length => use rest of the file. If duration is unknown, make
        // something up.
        if (part->length < 0)
            part->length = (len < 0 ? 1 : len) - part->offset;

        if (len > 0) {
            double partlen = part->offset + part->length;
            if (partlen > len) {
                MP_WARN(mpctx, "EDL: entry %d uses %f "
                       "seconds, but file has only %f seconds.\n",
                       n, partlen, len);
            }
        }

        // Add a chapter between each file.
        struct demux_chapter ch = {
            .pts = starttime,
            .name = talloc_strdup(chapters, part->filename),
        };
        MP_TARRAY_APPEND(NULL, chapters, num_chapters, ch);

        // Also copy the source file's chapters for the relevant parts
        copy_chapters(&chapters, &num_chapters, source, part->offset,
                      part->length, starttime);

        timeline[n] = (struct timeline_part) {
            .start = starttime,
            .source_start = part->offset,
            .source = source,
        };

        starttime += part->length;
    }
    timeline[parts->num_parts] = (struct timeline_part) {.start = starttime};
    mpctx->timeline = timeline;
    mpctx->num_timeline_parts = parts->num_parts;
    mpctx->chapters = chapters;
    mpctx->num_chapters = num_chapters;
    mpctx->track_layout = mpctx->timeline[0].source;
    return;

error:
    talloc_free(timeline);
    talloc_free(chapters);
}

// For security, don't allow relative or absolute paths, only plain filenames.
// Also, make these filenames relative to the edl source file.
static void fix_filenames(struct tl_parts *parts, char *source_path)
{
    struct bstr dirname = mp_dirname(source_path);
    for (int n = 0; n < parts->num_parts; n++) {
        struct tl_part *part = &parts->parts[n];
        char *filename = mp_basename(part->filename); // plain filename only
        part->filename = mp_path_join(parts, dirname, bstr0(filename));
    }
}

void build_mpv_edl_timeline(struct MPContext *mpctx)
{
    struct tl_parts *parts = parse_edl(mpctx->demuxer->file_contents);
    if (!parts) {
        MP_ERR(mpctx, "Error in EDL.\n");
        return;
    }
    // Source is .edl and not edl:// => don't allow arbitrary paths
    if (mpctx->demuxer->stream->uncached_type != STREAMTYPE_EDL)
        fix_filenames(parts, mpctx->demuxer->filename);
    build_timeline(mpctx, parts);
    talloc_free(parts);
}

static int try_open_file(struct demuxer *demuxer, enum demux_check check)
{
    struct stream *s = demuxer->stream;
    if (s->uncached_type == STREAMTYPE_EDL) {
        demuxer->file_contents = bstr0(s->path);
        return 0;
    }
    if (check >= DEMUX_CHECK_UNSAFE) {
        if (!bstr_equals0(stream_peek(s, strlen(HEADER)), HEADER))
            return -1;
    }
    demuxer->file_contents = stream_read_complete(s, demuxer, 1000000);
    if (demuxer->file_contents.start == NULL)
        return -1;
    bstr_eatstart0(&demuxer->file_contents, HEADER);
    return 0;
}

const struct demuxer_desc demuxer_desc_edl = {
    .name = "edl",
    .desc = "Edit decision list",
    .type = DEMUXER_TYPE_EDL,
    .open = try_open_file,
};
