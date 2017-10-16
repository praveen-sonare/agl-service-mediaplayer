/*
 * Copyright (C) 2017 Konsulko Group
 * Author: Matt Ranostay <matt.ranostay@konsulko.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <glib.h>
#include <pthread.h>
#include <gst/gst.h>
#include <json-c/json.h>
#include "afm-common.h"

#define AFB_BINDING_VERSION 2
#include <afb/afb-binding.h>

static struct afb_event playlist_event;
static struct afb_event metadata_event;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static GList *playlist = NULL;
static GList *current_track = NULL;

typedef struct _CustomData {
	GstElement *playbin;
	gboolean playing;
	guint volume;
	gint64 position;
	gint64 duration;
} CustomData;

CustomData data = {
	.volume = 50,
	.position = GST_CLOCK_TIME_NONE,
	.duration = GST_CLOCK_TIME_NONE,
};

static json_object *populate_json(struct playlist_item *track)
{
	json_object *jresp = json_object_new_object();
	json_object *jstring = json_object_new_string(track->media_path);
	json_object_object_add(jresp, "path", jstring);

	if (track->title) {
		jstring = json_object_new_string(track->title);
		json_object_object_add(jresp, "title", jstring);
	}

	if (track->album) {
		jstring = json_object_new_string(track->album);
		json_object_object_add(jresp, "album", jstring);
	}

	if (track->artist) {
		jstring = json_object_new_string(track->artist);
		json_object_object_add(jresp, "artist", jstring);
	}

	if (track->genre) {
		jstring = json_object_new_string(track->genre);
		json_object_object_add(jresp, "genre", jstring);
	}

	if (track->duration > 0)
		json_object_object_add(jresp, "duration",
			       json_object_new_int64(track->duration));

	json_object_object_add(jresp, "index",
			       json_object_new_int(track->id));

	return jresp;
}

static gboolean populate_from_json(struct playlist_item *item, json_object *jdict)
{
	gboolean ret;
	json_object *val = NULL;

	ret = json_object_object_get_ex(jdict, "path", &val);
	if (!ret)
		return ret;
	item->media_path = g_strdup(json_object_get_string(val));

	ret = json_object_object_get_ex(jdict, "title", &val);
	if (ret) {
		item->title = g_strdup(json_object_get_string(val));
	}

	ret = json_object_object_get_ex(jdict, "album", &val);
	if (ret) {
		item->album = g_strdup(json_object_get_string(val));
	}

	ret = json_object_object_get_ex(jdict, "artist", &val);
	if (ret) {
		item->artist = g_strdup(json_object_get_string(val));
	}

	ret = json_object_object_get_ex(jdict, "genre", &val);
	if (ret) {
		item->genre = g_strdup(json_object_get_string(val));
	}

	ret = json_object_object_get_ex(jdict, "duration", &val);
	if (ret) {
		item->duration = json_object_get_int64(val);
	}

	return TRUE;
}

static int set_media_uri(struct playlist_item *item)
{
	if (!item || !item->media_path)
		return -ENOENT;

	gst_element_set_state(data.playbin, GST_STATE_NULL);

	g_object_set(data.playbin, "uri", item->media_path, NULL);

	data.position = GST_CLOCK_TIME_NONE;
	data.duration = GST_CLOCK_TIME_NONE;

	if (data.playing)
		gst_element_set_state(data.playbin, GST_STATE_PLAYING);

	g_object_set(data.playbin, "volume", data.volume / 100.0, NULL);

	return 0;
}

static void populate_playlist(json_object *jquery)
{
	int i, idx = 0;
	GList *list = g_list_last(playlist);

	if (list && list->data) {
		struct playlist_item *item = list->data;
		idx = item->id + 1;
	}

	for (i = 0; i < json_object_array_length(jquery); i++) {
		json_object *jdict = json_object_array_get_idx(jquery, i);
		struct playlist_item *item = g_malloc0(sizeof(*item));
		int ret;

		if (item == NULL)
			break;

		ret = populate_from_json(item, jdict);
		if (!ret) {
			g_free_playlist_item(item);
			continue;
		}

		item->id = idx++;
		playlist = g_list_append(playlist, item);
	}

	current_track = g_list_first(playlist);
	set_media_uri(current_track->data);
}

static json_object *populate_json_playlist(json_object *jresp)
{
	GList *l;
	json_object *jarray = json_object_new_array();

	for (l = playlist; l; l = l->next) {
		json_object *item = populate_json(l->data);
		json_object_array_add(jarray, item);
	}

	json_object_object_add(jresp, "list", jarray);

	return jresp;
}

static void audio_playlist(struct afb_req request)
{
	const char *value = afb_req_value(request, "list");
	json_object *jresp = NULL;

	pthread_mutex_lock(&mutex);

	if (value) {
		json_object *jquery;

		if (playlist) {
			g_list_free_full(playlist, g_free_playlist_item);
			playlist = NULL;
		}

		jquery = json_tokener_parse(value);
		populate_playlist(jquery);

		if (playlist == NULL)
	        afb_req_fail(request, "failed", "invalid playlist");
		else
			afb_req_success(request, NULL, NULL);

		json_object_put(jquery);
	} else {
		jresp = json_object_new_object();
		jresp = populate_json_playlist(jresp);

		afb_req_success(request, jresp, "Playlist results");
	}

	pthread_mutex_unlock(&mutex);
}

static int seek_track(int cmd)
{
	GList *item = (cmd == NEXT_CMD) ? current_track->next : current_track->prev;
	int ret;

	if (item == NULL)
		return -EINVAL;

	ret = set_media_uri(item->data);
	if (ret < 0)
		return -EINVAL;

	if (data.playing)
		gst_element_set_state(data.playbin, GST_STATE_PLAYING);

	current_track = item;

	return 0;
}

static int seek_stream(const char *value, int cmd)
{
	gint64 position, current = 0;

	if (value == NULL)
		return -EINVAL;

	position = strtoll(value, NULL, 10);

	if (cmd != SEEK_CMD) {
		gst_element_query_position (data.playbin, GST_FORMAT_TIME, &current);
		position = (current / GST_MSECOND) + (FASTFORWARD_CMD ? position : -position);
	}

	if (position < 0)
		position = 0;

	if (data.duration > 0 && position > data.duration)
		position = data.duration;

	return gst_element_seek_simple(data.playbin, GST_FORMAT_TIME,
				GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
				position * GST_MSECOND);
}

/* @value can be one of the following values:
 *   play     - go to playing transition
 *   pause    - go to pause transition
 *   previous - skip to previous track
 *   next     - skip to the next track
 *   seek     - go to position (in milliseconds)
 *
 *   fast-forward - skip forward in milliseconds
 *   rewind       - skip backward in milliseconds
 *
 *   pick-track   - select track via index number
 *   volume       - set volume between 0 - 100%
 */

static void controls(struct afb_req request)
{
	const char *value = afb_req_value(request, "value");
	const char *position = afb_req_value(request, "position");
	int cmd = get_command_index(value);

	if (!value) {
		afb_req_fail(request, "failed", "no value was passed");
		return;
	}

	pthread_mutex_lock(&mutex);
	errno = 0;

	switch (cmd) {
	case PLAY_CMD:
		gst_element_set_state(data.playbin, GST_STATE_PLAYING);
		data.playing = TRUE;
		break;
	case PAUSE_CMD:
		gst_element_set_state(data.playbin, GST_STATE_PAUSED);
		data.playing = FALSE;
		break;
	case PREVIOUS_CMD:
	case NEXT_CMD:
		seek_track(cmd);
		break;
	case SEEK_CMD:
	case FASTFORWARD_CMD:
	case REWIND_CMD:
		seek_stream(position, cmd);
		break;
	case PICKTRACK_CMD: {
		const char *parameter = afb_req_value(request, "index");
		long int idx = strtol(parameter, NULL, 10);
		GList *list = NULL;

		if (idx == 0 && !errno) {
			afb_req_fail(request, "failed", "invalid index");
			pthread_mutex_unlock(&mutex);
			return;
		}

		list = find_media_index(playlist, idx);
		if (list != NULL) {
			struct playlist_item *item = list->data;
			set_media_uri(item);
			current_track = list;
		} else {
			afb_req_fail(request, "failed", "couldn't find index");
			pthread_mutex_unlock(&mutex);
			return;
		}

		break;
	}
	case VOLUME_CMD: {
		const char *parameter = afb_req_value(request, "volume");
		long int volume = strtol(parameter, NULL, 10);

		if (volume == 0 && !errno) {
			afb_req_fail(request, "failed", "invalid volume");
			pthread_mutex_unlock(&mutex);
			return;
		}

		if (volume < 0)
			volume = 0;
		if (volume > 100)
			volume = 100;

		g_object_set(data.playbin, "volume", volume / 100.0, NULL);

		data.volume = volume;

		break;
	}
	default:
		afb_req_fail(request, "failed", "unknown command");
		pthread_mutex_unlock(&mutex);
		return;
	}

	afb_req_success(request, NULL, NULL);
	pthread_mutex_unlock(&mutex);
}

static void metadata(struct afb_req request)
{
	struct playlist_item *track;
	json_object *jresp;

	pthread_mutex_lock(&mutex);

	if (current_track == NULL || current_track->data == NULL) {
		afb_req_fail(request, "failed", "No playlist");
		pthread_mutex_unlock(&mutex);
		return;
	}

	track = current_track->data;
	jresp = populate_json(track);

	if (data.duration != GST_CLOCK_TIME_NONE)
		json_object_object_add(jresp, "duration",
			       json_object_new_int64(data.duration / GST_MSECOND));

	if (data.position != GST_CLOCK_TIME_NONE)
		json_object_object_add(jresp, "position",
			       json_object_new_int64(data.position / GST_MSECOND));

	json_object_object_add(jresp, "volume",
			       json_object_new_int(data.volume));

	pthread_mutex_unlock(&mutex);

	afb_req_success(request, jresp, "Metadata results");
}

static void subscribe(struct afb_req request)
{
	const char *value = afb_req_value(request, "value");

	if (!strcasecmp(value, "metadata")) {
		afb_req_subscribe(request, metadata_event);
		afb_req_success(request, NULL, NULL);
		return;
	} else if (!strcasecmp(value, "playlist")) {
		afb_req_subscribe(request, playlist_event);
		afb_req_success(request, NULL, NULL);
		return;
	}

	afb_req_fail(request, "failed", "Invalid event");
}

static void unsubscribe(struct afb_req request)
{
	const char *value = afb_req_value(request, "value");

	if (!strcasecmp(value, "metadata")) {
		afb_req_unsubscribe(request, metadata_event);
		afb_req_success(request, NULL, NULL);
		return;
	} else if (!strcasecmp(value, "playlist")) {
		afb_req_unsubscribe(request, playlist_event);
		afb_req_success(request, NULL, NULL);
		return;
	}

	afb_req_fail(request, "failed", "Invalid event");
}

static gboolean handle_message(GstBus *bus, GstMessage *msg, CustomData *data)
{
	switch (GST_MESSAGE_TYPE (msg)) {
	case GST_MESSAGE_EOS: {
		int ret;

		pthread_mutex_lock(&mutex);

		data->position = GST_CLOCK_TIME_NONE;
		data->duration = GST_CLOCK_TIME_NONE;

		ret = seek_track(NEXT_CMD);
		if (ret < 0) {
			data->playing = FALSE;
			current_track = playlist;
		} else if (data->playing) {
			gst_element_set_state(data->playbin, GST_STATE_PLAYING);
		}

		pthread_mutex_unlock(&mutex);
		break;
	}
	case GST_MESSAGE_DURATION:
		data->duration = GST_CLOCK_TIME_NONE;
		break;
	default:
		break;
	}

	return TRUE;
}

static gboolean position_event(CustomData *data)
{
	struct playlist_item *track;
	json_object *jresp = NULL;

	pthread_mutex_lock(&mutex);

	if (!data->playing || current_track == NULL) {
		pthread_mutex_unlock(&mutex);
		return TRUE;
	}

	track = current_track->data;
	jresp = populate_json(track);

	if (!GST_CLOCK_TIME_IS_VALID(data->duration))
		gst_element_query_duration(data->playbin,
					GST_FORMAT_TIME, &data->duration);

	gst_element_query_position(data->playbin,
					GST_FORMAT_TIME, &data->position);

	json_object_object_add(jresp, "duration",
			       json_object_new_int64(data->duration / GST_MSECOND));
	json_object_object_add(jresp, "position",
			       json_object_new_int64(data->position / GST_MSECOND));

	pthread_mutex_unlock(&mutex);

        afb_event_push(metadata_event, jresp);

	return TRUE;
}

static void *gstreamer_loop_thread(void *ptr)
{
	GstBus *bus;
	json_object *response;
	int ret;

	gst_init(NULL, NULL);

	data.playbin = gst_element_factory_make("playbin", "playbin");
	if (!data.playbin) {
		AFB_ERROR("Cannot create playbin");
		exit(1);
	}

	bus = gst_element_get_bus(data.playbin);
	gst_bus_add_watch(bus, (GstBusFunc) handle_message, &data);
	g_timeout_add_seconds(1, (GSourceFunc) position_event, &data);

	ret = afb_service_call_sync("mediascanner", "media_result", NULL, &response);
	if (!ret) {
		json_object *val = NULL;
		gboolean ret;

		ret = json_object_object_get_ex(response, "response", &val);
		if (ret)
			ret = json_object_object_get_ex(val, "Media", &val);

		if (ret)
			populate_playlist(val);
	}
	json_object_put(response);

	g_main_loop_run(g_main_loop_new(NULL, FALSE));

	return NULL;
}

static void onevent(const char *event, struct json_object *object)
{
	json_object *jresp = NULL;

	if (!g_strcmp0(event, "mediascanner/media_added")) {
		json_object *val = NULL;
		gboolean ret;

		pthread_mutex_lock(&mutex);

		ret = json_object_object_get_ex(object, "Media", &val);
		if (ret)
			populate_playlist(val);

	} else if (!g_strcmp0(event, "mediascanner/media_removed")) {
		json_object *val = NULL;
		const char *path;
		GList *l = playlist;
		gboolean ret;

		ret = json_object_object_get_ex(object, "Path", &val);
		if (!ret)
			return;

		path = json_object_get_string(val);

		pthread_mutex_lock(&mutex);

		while (l) {
			struct playlist_item *item = l->data;

			l = l->next;

			if (!strncasecmp(path, item->media_path, strlen(path))) {
				playlist = g_list_remove(playlist, item);
				g_free_playlist_item(item);

				if (current_track->data == item) {
					current_track = NULL;
					gst_element_set_state(data.playbin, GST_STATE_NULL);
				}
			}
		}

		current_track = g_list_first(playlist);

	} else {
		AFB_ERROR("Invalid event: %s", event);
		return;
	}

	jresp = json_object_new_object();
	jresp = populate_json_playlist(jresp);

	pthread_mutex_unlock(&mutex);

        afb_event_push(playlist_event, jresp);
}

static int init() {
	pthread_t thread_id;
	json_object *response, *query;
	int ret;

	ret = afb_daemon_require_api("mediascanner", 1);
	if (ret < 0) {
		AFB_ERROR("Cannot request mediascanner");
		return ret;
	}

	query = json_object_new_object();
	json_object_object_add(query, "value", json_object_new_string("media_added"));

	ret = afb_service_call_sync("mediascanner", "subscribe", query, &response);
	json_object_put(response);

	if (ret < 0) {
		AFB_ERROR("Cannot subscribe to mediascanner media_added event");
		return ret;
	}

	query = json_object_new_object();
	json_object_object_add(query, "value", json_object_new_string("media_removed"));

	ret = afb_service_call_sync("mediascanner", "subscribe", query, &response);
	json_object_put(response);

	if (ret < 0) {
		AFB_ERROR("Cannot subscribe to mediascanner media_remove event");
		return ret;
	}

	metadata_event = afb_daemon_make_event("metadata");
	playlist_event = afb_daemon_make_event("playlist");

	return pthread_create(&thread_id, NULL, gstreamer_loop_thread, NULL);
}

static const struct afb_verb_v2 binding_verbs[] = {
	{ .verb = "playlist",     .callback = audio_playlist, .info = "Get/set playlist" },
	{ .verb = "controls",     .callback = controls,       .info = "Audio controls" },
	{ .verb = "metadata",     .callback = metadata,       .info = "Get metadata of current track" },
	{ .verb = "subscribe",    .callback = subscribe,      .info = "Subscribe to GStreamer events" },
	{ .verb = "unsubscribe",  .callback = unsubscribe,    .info = "Unsubscribe to GStreamer events" },
	{ }
};

/*
 * binder API description
 */
const struct afb_binding_v2 afbBindingV2 = {
	.api = "mediaplayer",
	.specification = "Mediaplayer API",
	.verbs = binding_verbs,
	.onevent = onevent,
	.init = init,
};
