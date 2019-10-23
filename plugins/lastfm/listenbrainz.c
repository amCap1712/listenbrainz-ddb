/*
    ListenBrainz scrobbler plugin for DeaDBeeF Player
    TODO: Add copyright

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>
#include <math.h>
#include "../../deadbeef.h"

#define trace(...) { deadbeef->log_detailed (&plugin.plugin, 0, __VA_ARGS__); }

#define LFM_TESTMODE 0
#define LFM_IGNORE_RULES 0
#define LFM_NOSEND 0

static DB_misc_t plugin;
static DB_functions_t *deadbeef;

#define LFM_CLIENTID "ddb"
#define SCROBBLER_URL_LISTENBRAINZ "https://api.listenbrainz.org"

#ifdef __MINGW32__
#define LOOKUP_URL_FORMAT "cmd /c start http://www.last.fm/music/%s/_/%s"
#else
#define LOOKUP_URL_FORMAT "xdg-open 'http://www.last.fm/music/%s/_/%s' &"
#endif

static char listenbrainz_pass[100];

static char listenbrainz_submission_url[256];

static uintptr_t listenbrainz_mutex;
static uintptr_t listenbrainz_cond;
static int listenbrainz_stopthread;
static intptr_t listenbrainz_tid;

#define META_FIELD_SIZE 200

DB_plugin_t *
listenbrainz_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

#define MAX_REPLY 4096
static char listenbrainz_reply[MAX_REPLY];
static int listenbrainz_reply_sz;
static char listenbrainz_err[CURL_ERROR_SIZE];

#define LFM_SUBMISSION_QUEUE_SIZE 50

typedef struct {
    DB_playItem_t *it;
    time_t started_timestamp;
    float playtime;
} subm_item_t;

static subm_item_t listenbrainz_subm_queue[LFM_SUBMISSION_QUEUE_SIZE];

static void
listenbrainz_update_auth (void) {
    deadbeef->conf_lock ();
    const char *pass = deadbeef->conf_get_str_fast ("listenbrainz.usertoken", "");
    if (strcmp (pass, listenbrainz_pass))
        strcpy (listenbrainz_pass, pass);
    deadbeef->conf_unlock ();
}

static size_t
listenbrainz_curl_res (void *ptr, size_t size, size_t nmemb, void *stream)
{
    if (listenbrainz_stopthread) {
        trace ("listenbrainz: listenbrainz_curl_res: aborting current request\n");
        return 0;
    }
    int len = size * nmemb;
    if (listenbrainz_reply_sz + len >= MAX_REPLY) {
        trace ("reply is too large. stopping.\n");
        return 0;
    }
    memcpy (listenbrainz_reply + listenbrainz_reply_sz, ptr, len);
    listenbrainz_reply_sz += len;
//    char s[size*nmemb+1];
//    memcpy (s, ptr, size*nmemb);
//    s[size*nmemb] = 0;
//    trace ("got from net: %s\n", s);
    return len;
}

static int
listenbrainz_curl_control (void *stream, double dltotal, double dlnow, double ultotal, double ulnow) {
    if (listenbrainz_stopthread) {
        trace ("listenbrainz: aborting current request\n");
        return -1;
    }
    return 0;
}
static int
curl_req_send (const char *req, const char *post) {
    trace ("sending request: %s\n", req);
    CURL *curl;
    curl = curl_easy_init ();
    if (!curl) {
        trace ("listenbrainz: failed to init curl\n");
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, req);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, listenbrainz_curl_res);
    memset(listenbrainz_err, 0, sizeof(listenbrainz_err));
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, listenbrainz_err);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt (curl, CURLOPT_PROGRESSFUNCTION, listenbrainz_curl_control);
    char ua[100];
    deadbeef->conf_get_str ("network.http_user_agent", "deadbeef", ua, sizeof (ua));
    curl_easy_setopt (curl, CURLOPT_USERAGENT, ua);
    curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 0);
    if (post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(post));
    }
    if (deadbeef->conf_get_int ("network.proxy", 0)) {
        deadbeef->conf_lock ();
        curl_easy_setopt (curl, CURLOPT_PROXY, deadbeef->conf_get_str_fast ("network.proxy.address", ""));
        curl_easy_setopt (curl, CURLOPT_PROXYPORT, deadbeef->conf_get_int ("network.proxy.port", 8080));
        const char *type = deadbeef->conf_get_str_fast ("network.proxy.type", "HTTP");
        int curlproxytype = CURLPROXY_HTTP;
        if (!strcasecmp (type, "HTTP")) {
            curlproxytype = CURLPROXY_HTTP;
        }
#if LIBCURL_VERSION_MINOR >= 19 && LIBCURL_VERSION_PATCH >= 4
            else if (!strcasecmp (type, "HTTP_1_0")) {
            curlproxytype = CURLPROXY_HTTP_1_0;
        }
#endif
#if LIBCURL_VERSION_MINOR >= 15 && LIBCURL_VERSION_PATCH >= 2
            else if (!strcasecmp (type, "SOCKS4")) {
            curlproxytype = CURLPROXY_SOCKS4;
        }
#endif
        else if (!strcasecmp (type, "SOCKS5")) {
            curlproxytype = CURLPROXY_SOCKS5;
        }
#if LIBCURL_VERSION_MINOR >= 18 && LIBCURL_VERSION_PATCH >= 0
        else if (!strcasecmp (type, "SOCKS4A")) {
            curlproxytype = CURLPROXY_SOCKS4A;
        }
        else if (!strcasecmp (type, "SOCKS5_HOSTNAME")) {
            curlproxytype = CURLPROXY_SOCKS5_HOSTNAME;
        }
#endif
        curl_easy_setopt (curl, CURLOPT_PROXYTYPE, curlproxytype);

        const char *proxyuser = deadbeef->conf_get_str_fast ("network.proxy.username", "");
        const char *proxypass = deadbeef->conf_get_str_fast ("network.proxy.usertoken", "");
        if (*proxyuser || *proxypass) {
#if LIBCURL_VERSION_MINOR >= 19 && LIBCURL_VERSION_PATCH >= 1
            curl_easy_setopt (curl, CURLOPT_PROXYUSERNAME, proxyuser);
            curl_easy_setopt (curl, CURLOPT_PROXYUSERNAME, proxypass);
#else
            char pwd[200];
            snprintf (pwd, sizeof (pwd), "%s:%s", proxyuser, proxypass);
            curl_easy_setopt (curl, CURLOPT_PROXYUSERPWD, pwd);
#endif
        }
        deadbeef->conf_unlock ();
    }
    int status = curl_easy_perform(curl);
    curl_easy_cleanup (curl);
    if (!status) {
        listenbrainz_reply[listenbrainz_reply_sz] = 0;
    }
    if (status != 0) {
        trace ("curl request failed, err:\n%s\n", listenbrainz_err);
    }
    return status;
}

static void
curl_req_cleanup (void) {
    listenbrainz_reply_sz = 0;
}

static int
listenbrainz_fetch_song_info (DB_playItem_t *song, float playtime, char *a, char *t, char *b, float *l, char *n, char *m) {
    if (deadbeef->conf_get_int ("listenbrainz.prefer_album_artist", 0)) {
        if (!deadbeef->pl_get_meta (song, "band", a, META_FIELD_SIZE)) {
            if (!deadbeef->pl_get_meta (song, "album artist", a, META_FIELD_SIZE)) {
                if (!deadbeef->pl_get_meta (song, "albumartist", a, META_FIELD_SIZE)) {
                    if (!deadbeef->pl_get_meta (song, "artist", a, META_FIELD_SIZE)) {
                        return -1;
                    }
                }
            }
        }
    }
    else {
        if (!deadbeef->pl_get_meta (song, "artist", a, META_FIELD_SIZE)) {
            if (!deadbeef->pl_get_meta (song, "band", a, META_FIELD_SIZE)) {
                if (!deadbeef->pl_get_meta (song, "album artist", a, META_FIELD_SIZE)) {
                    if (!deadbeef->pl_get_meta (song, "albumartist", a, META_FIELD_SIZE)) {
                        return -1;
                    }
                }
            }
        }
    }
    if (!deadbeef->pl_get_meta (song, "title", t, META_FIELD_SIZE)) {
        return -1;
    }
    if (!deadbeef->pl_get_meta (song, "album", b, META_FIELD_SIZE)) {
        *b = 0;
    }
    *l = deadbeef->pl_get_item_duration (song);
    if (*l <= 0) {
        *l = playtime;
    }
    if (!deadbeef->pl_get_meta (song, "track", n, META_FIELD_SIZE)) {
        *n = 0;
    }
    if (!deadbeef->conf_get_int ("listenbrainz.mbid", 0) || !deadbeef->pl_get_meta (song, "musicbrainz_trackid", m, META_FIELD_SIZE)) {
        *m = 0;
    }
    return 0;
}

// subm is submission idx, or -1 for nowplaying
// returns number of bytes added, or -1
static int
listenbrainz_format_uri (int subm, DB_playItem_t *song, char *out, int outl, time_t started_timestamp, float playtime) {
    if (subm > 50) {
        trace ("listenbrainz: it's only allowed to send up to 50 submissions at once (got idx=%d)\n", subm);
        return -1;
    }
    int sz = outl;
    char a[META_FIELD_SIZE]; // artist
    char t[META_FIELD_SIZE]; // title
    char b[META_FIELD_SIZE]; // album
    float l; // duration
    char n[META_FIELD_SIZE]; // tracknum
    char m[META_FIELD_SIZE]; // muzicbrainz id

    char ka[6] = "a";
    char kt[6] = "t";
    char kb[6] = "b";
    char kl[6] = "l";
    char kn[6] = "n";
    char km[6] = "m";

    if (subm >= 0) {
        snprintf (ka+1, 5, "[%d]", subm);
        strcpy (kt+1, ka+1);
        strcpy (kb+1, ka+1);
        strcpy (kl+1, ka+1);
        strcpy (kn+1, ka+1);
        strcpy (km+1, ka+1);
    }

    if (listenbrainz_fetch_song_info (song, playtime, a, t, b, &l, n, m) == 0) {
//        trace ("playtime: %f\nartist: %s\ntitle: %s\nalbum: %s\nduration: %f\ntracknum: %s\n---\n", song->playtime, a, t, b, l, n);
    }
    else {
//        trace ("file %s doesn't have enough tags to submit to last.fm\n", song->fname);
        return -1;
    }

    if (listenbrainz_add_keyvalue_uri_encoded (&out, &outl, ka, a) < 0) {
//        trace ("failed to add %s=%s\n", ka, a);
        return -1;
    }
    if (listenbrainz_add_keyvalue_uri_encoded (&out, &outl, kt, t) < 0) {
//        trace ("failed to add %s=%s\n", kt, t);
        return -1;
    }
    if (listenbrainz_add_keyvalue_uri_encoded (&out, &outl, kb, b) < 0) {
//        trace ("failed to add %s=%s\n", kb, b);
        return -1;
    }
    if (listenbrainz_add_keyvalue_uri_encoded (&out, &outl, kn, n) < 0) {
//        trace ("failed to add %s=%s\n", kn, n);
        return -1;
    }
    if (listenbrainz_add_keyvalue_uri_encoded (&out, &outl, km, m) < 0) {
//        trace ("failed to add %s=%s\n", km, m);
        return -1;
    }
    int processed;
    processed = snprintf (out, outl, "%s=%d&", kl, (int)l);
    if (processed > outl) {
//        trace ("failed to add %s=%d\n", kl, (int)l);
        return -1;
    }
    out += processed;
    outl -= processed;
    if (subm >= 0) {
        processed = snprintf (out, outl, "i[%d]=%d&o[%d]=P&r[%d]=&", subm, (int)started_timestamp, subm, subm);
        if (processed > outl) {
//            trace ("failed to add i[%d]=%d&o[%d]=P&r[%d]=&\n", subm, (int)song->started_timestamp, subm, subm);
            return -1;
        }
        out += processed;
        outl -= processed;
    }

    return sz - outl;
}

static int
listenbrainz_songstarted (ddb_event_track_t *ev, uintptr_t data) {
    trace ("listenbrainz songstarted %p\n", ev->track);
    if (!deadbeef->conf_get_int ("listenbrainz.enable", 0)) {
        return 0;
    }
    deadbeef->mutex_lock (listenbrainz_mutex);
    if (listenbrainz_format_uri (-1, ev->track, listenbrainz_nowplaying, sizeof (listenbrainz_nowplaying), ev->started_timestamp, 120) < 0) {
        listenbrainz_nowplaying[0] = 0;
    }
//    trace ("%s\n", listenbrainz_nowplaying);
    deadbeef->mutex_unlock (listenbrainz_mutex);
    if (listenbrainz_nowplaying[0]) {
        deadbeef->cond_signal (listenbrainz_cond);
    }

    return 0;
}

static int
listenbrainz_songchanged (ddb_event_trackchange_t *ev, uintptr_t data) {
    if (!deadbeef->conf_get_int ("listenbrainz.enable", 0)) {
        return 0;
    }
    // previous track must exist
    if (!ev->from) {
        return 0;
    }
    trace ("listenbrainz songfinished %s\n", deadbeef->pl_find_meta (ev->from, ":URI"));
#if !LFM_IGNORE_RULES
    // check submission rules
    // duration/playtime must be >= 30 sec
    float dur = deadbeef->pl_get_item_duration (ev->from);
    if (dur < 30 && ev->playtime < 30) {
        // the listenbrainz.send_tiny_tracks option can override this rule
        // only if the track played fully, and has determined duration
        if (!(dur > 0 && fabs (ev->playtime - dur) < 1.f && deadbeef->conf_get_int ("listenbrainz.submit_tiny_tracks", 0))) {
            trace ("track duration is %f sec, playtime if %f sec. not eligible for submission\n", dur, ev->playtime);
            return 0;
        }
    }
    // must be played for >=240sec or half the total time
    if (ev->playtime < 240 && ev->playtime < dur/2) {
        trace ("track playtime=%f seconds. not eligible for submission\n", ev->playtime);
        return 0;
    }

#endif

    if (!deadbeef->pl_meta_exists (ev->from, "artist")
        || !deadbeef->pl_meta_exists (ev->from, "title")
            ) {
        trace ("listenbrainz: not enough metadata for submission, artist=%s, title=%s, album=%s\n", deadbeef->pl_find_meta (ev->from, "artist"), deadbeef->pl_find_meta (ev->from, "title"), deadbeef->pl_find_meta (ev->from, "album"));
        return 0;
    }
    deadbeef->mutex_lock (listenbrainz_mutex);
    // find free place in queue
    for (int i = 0; i < LFM_SUBMISSION_QUEUE_SIZE; i++) {
        if (!listenbrainz_subm_queue[i].it) {
            trace ("listenbrainz: song is now in queue for submission\n");
            listenbrainz_subm_queue[i].it = ev->from;
            listenbrainz_subm_queue[i].started_timestamp = ev->started_timestamp;
            listenbrainz_subm_queue[i].playtime = ev->playtime;
            deadbeef->pl_item_ref (ev->from);
            break;
        }
    }
    deadbeef->mutex_unlock (listenbrainz_mutex);
    deadbeef->cond_signal (listenbrainz_cond);

    return 0;
}

static void
listenbrainz_send_submissions (void) {
    trace ("listenbrainz_send_submissions\n");
    int i;
    char req[1024*50];
    int idx = 0;
    char *r = req;
    int len = sizeof (req);
    int res;
    deadbeef->mutex_lock (listenbrainz_mutex);
    for (i = 0; i < LFM_SUBMISSION_QUEUE_SIZE; i++) {
        if (listenbrainz_subm_queue[i].it) {
            res = listenbrainz_format_uri (idx, listenbrainz_subm_queue[i].it, r, len, listenbrainz_subm_queue[i].started_timestamp, listenbrainz_subm_queue[i].playtime);
            if (res < 0) {
                trace ("listenbrainz: failed to format uri\n");
                return;
            }
            len -= res;
            r += res;
            idx++;
        }
    }
    deadbeef->mutex_unlock (listenbrainz_mutex);
    if (!idx) {
        return;
    }

    *listenbrainz_submission_url = deadbeef->conf_get_str_fast ("listenbrainz.scrobbler_url", SCROBBLER_URL_LFM);

    res = snprintf (r, len, "s=%s&", listenbrainz_sess);
    if (res > len) {
        return;
    }
    trace ("submission req string:\n%s\n", req);
#if !LFM_NOSEND
    for (int attempts = 2; attempts > 0; attempts--) {
        int status = curl_req_send (listenbrainz_submission_url, req);
        if (!status) {
            if (strncmp (listenbrainz_reply, "OK", 2)) {
                trace ("submission failed, response:\n%s\n", listenbrainz_reply);
                if (!strncmp (listenbrainz_reply, "BADSESSION", 7)) {
                    trace ("got badsession; trying to restore session...\n");
                    listenbrainz_sess[0] = 0;
                    curl_req_cleanup ();
                    if (auth () < 0) {
                        trace ("fail!\n");
                        break; // total fail
                    }
                    trace ("success! retrying send nowplaying...\n");
                    res = snprintf (r, len, "s=%s&", listenbrainz_sess);
                    continue; // retry with new session
                }
            }
            else {
                trace ("submission successful, response:\n%s\n", listenbrainz_reply);
                deadbeef->mutex_lock (listenbrainz_mutex);
                for (i = 0; i < LFM_SUBMISSION_QUEUE_SIZE; i++) {
                    if (listenbrainz_subm_queue[i].it) {
                        deadbeef->pl_item_unref (listenbrainz_subm_queue[i].it);
                        listenbrainz_subm_queue[i].it = NULL;
                        listenbrainz_subm_queue[i].started_timestamp = 0;
                    }
                }
                deadbeef->mutex_unlock (listenbrainz_mutex);
            }
        }
        curl_req_cleanup ();
        break;
    }
#else
    trace ("submission successful (NOSEND=1):\n");
    deadbeef->mutex_lock (listenbrainz_mutex);
    for (i = 0; i < LFM_SUBMISSION_QUEUE_SIZE; i++) {
        if (listenbrainz_subm_queue[i].it) {
            deadbeef->pl_item_unref (listenbrainz_subm_queue[i].it);
            listenbrainz_subm_queue[i].it = NULL;
            listenbrainz_subm_queue[i].started_timestamp = 0;

        }
    }
    deadbeef->mutex_unlock (listenbrainz_mutex);
#endif
}

static void
listenbrainz_thread (void *ctx) {
    //trace ("listenbrainz_thread started\n");
    for (;;) {
        if (listenbrainz_stopthread) {
            deadbeef->mutex_unlock (listenbrainz_mutex);
            trace ("listenbrainz_thread end\n");
            return;
        }
        trace ("listenbrainz wating for cond...\n");
        deadbeef->cond_wait (listenbrainz_cond, listenbrainz_mutex);
        if (listenbrainz_stopthread) {
            deadbeef->mutex_unlock (listenbrainz_mutex);
            trace ("listenbrainz_thread end[2]\n");
            return;
        }
        trace ("cond signalled!\n");
        deadbeef->mutex_unlock (listenbrainz_mutex);

        if (!deadbeef->conf_get_int ("listenbrainz.enable", 0)) {
            continue;
        }
        trace ("listenbrainz sending nowplaying...\n");
        listenbrainz_send_submissions ();
        // try to send nowplaying
        if (listenbrainz_nowplaying[0] && !deadbeef->conf_get_int ("listenbrainz.disable_np", 0)) {
            listenbrainz_send_nowplaying ();
        }
    }
}

static int
listenbrainz_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
        case DB_EV_SONGSTARTED:
            listenbrainz_songstarted ((ddb_event_track_t *)ctx, 0);
            break;
        case DB_EV_SONGCHANGED:
            listenbrainz_songchanged ((ddb_event_trackchange_t *)ctx, 0);
            break;
    }
    return 0;
}

static int
listenbrainz_start (void) {
    if (listenbrainz_mutex) {
        return -1;
    }
    listenbrainz_stopthread = 0;
    listenbrainz_mutex = deadbeef->mutex_create_nonrecursive ();
    listenbrainz_cond = deadbeef->cond_create ();
    listenbrainz_tid = deadbeef->thread_start (listenbrainz_thread, NULL);

    return 0;
}

static int
listenbrainz_stop (void) {
    trace ("listenbrainz_stop\n");
    if (listenbrainz_mutex) {
        listenbrainz_stopthread = 1;

        trace ("listenbrainz_stop signalling cond\n");
        deadbeef->cond_signal (listenbrainz_cond);
        trace ("waiting for thread to finish\n");
        deadbeef->thread_join (listenbrainz_tid);
        listenbrainz_tid = 0;
        deadbeef->cond_free (listenbrainz_cond);
        deadbeef->mutex_free (listenbrainz_mutex);
    }
    return 0;
}

static int
listenbrainz_action_lookup (DB_plugin_action_t *action, int ctx)
{
    char *command = NULL;
    DB_playItem_t *it = NULL;
    char artist[META_FIELD_SIZE];
    char title[META_FIELD_SIZE];

    if (ctx == DDB_ACTION_CTX_SELECTION) {
        // find first selected
        ddb_playlist_t *plt = deadbeef->plt_get_curr ();
        if (plt) {
            it = deadbeef->plt_get_first (plt, PL_MAIN);
            while (it) {
                if (deadbeef->pl_is_selected (it)) {
                    break;
                }
                DB_playItem_t *next = deadbeef->pl_get_next (it, PL_MAIN);
                deadbeef->pl_item_unref (it);
                it = next;
            }
            deadbeef->plt_unref (plt);
        }
    }
    else if (ctx == DDB_ACTION_CTX_NOWPLAYING) {
        it = deadbeef->streamer_get_playing_track ();
    }
    if (!it) {
        goto out;
    }

    if (!deadbeef->pl_get_meta (it, "artist", artist, sizeof (artist))) {
        goto out;
    }
    if (!deadbeef->pl_get_meta (it, "title", title, sizeof (title))) {
        goto out;
    }

    int la = strlen (artist) * 3 + 1;
    int lt = strlen (title) * 3 + 1;
    char *eartist = alloca (la);
    char *etitle = alloca (lt);

    if (-1 == listenbrainz_uri_encode (eartist, la, artist)) {
        goto out;
    }

    if (-1 == listenbrainz_uri_encode (etitle, lt, title)) {
        goto out;
    }

    if (-1 == asprintf (&command, LOOKUP_URL_FORMAT, eartist, etitle)) {
        goto out;
    }

    int res = system (command);
    out:
    if (it) {
        deadbeef->pl_item_unref (it);
    }
    if (command) {
        free (command);
    }
    return 0;
}

static DB_plugin_action_t lookup_action = {
        .title = "Lookup On Last.fm",
        .name = "listenbrainz_lookup",
        .flags = DB_ACTION_SINGLE_TRACK | DB_ACTION_ADD_MENU,
        .callback2 = listenbrainz_action_lookup,
        .next = NULL
};

static DB_plugin_action_t *
listenbrainz_get_actions (DB_playItem_t *it)
{
    deadbeef->pl_lock ();
    if (!it ||
        !deadbeef->pl_meta_exists (it, "artist") ||
        !deadbeef->pl_meta_exists (it, "title"))
    {
        lookup_action.flags |= DB_ACTION_DISABLED;
    }
    else
    {
        lookup_action.flags &= ~DB_ACTION_DISABLED;
    }
    deadbeef->pl_unlock ();
    return &lookup_action;
}

static const char settings_dlg[] =
        "property \"Enable scrobbler\" checkbox listenbrainz.enable 0;"
        "property User token entry listenbrainz.usertoken \"\";"
        "property \"Scrobble URL\" entry listenbrainz.scrobbler_url \""SCROBBLER_URL_LISTENBRAINZ"\";"
;

// define plugin interface
static DB_misc_t plugin = {
    //TODO: add copyright
        DDB_PLUGIN_SET_API_VERSION
        .plugin.version_major = 1,
        .plugin.version_minor = 0,
        .plugin.type = DB_PLUGIN_MISC,
        .plugin.flags = DDB_PLUGIN_FLAG_LOGGING,
        .plugin.name = "ListenBrainz",
        .plugin.descr = "Sends your listens history to your ListenBrainz account",
        .plugin.copyright =
        "ListenBrainz scrobbler plugin for DeaDBeeF Player\n"
        "\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
        ,
        .plugin.website = "http://deadbeef.sf.net",
        .plugin.start = listenbrainz_start,
        .plugin.stop = listenbrainz_stop,
        .plugin.configdialog = settings_dlg,
        .plugin.get_actions = listenbrainz_get_actions,
        .plugin.message = listenbrainz_message,
};
