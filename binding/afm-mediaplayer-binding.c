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
#include <gio/gio.h>
#include <pthread.h>
#include <gst/gst.h>
#include <gst/tag/tag.h>
#include <json-c/json.h>
#include "afm-common.h"

#define AFB_BINDING_VERSION 3
#include <afb/afb-binding.h>

static afb_event_t playlist_event;
static afb_event_t metadata_event;
static GMutex mutex;

static GList *playlist = NULL;
static GList *metadata_track = NULL;
static GList *current_track = NULL;

static json_object *populate_json_metadata(void);

enum {
        LOOP_OFF,
        LOOP_PLAYLIST,
        LOOP_TRACK,
	LOOP_NUM_TYPES,
};

static const char * const LOOP_STATES[LOOP_NUM_TYPES] = {
        "off",
        "playlist",
        "track",
};

typedef struct _CustomData {
	GstElement *playbin, *fake_sink, *audio_sink;
	gboolean playing;
	int loop_state;
	gboolean one_time;
	long int volume;
	gint64 position;
	gint64 duration;
	afb_api_t api;

	/* avrcp */
	gboolean avrcp_connected;
} CustomData;

CustomData data = {
	.volume = 50,
	.position = GST_CLOCK_TIME_NONE,
	.duration = GST_CLOCK_TIME_NONE,
};


static int find_loop_state_idx(const char *state)
{
	int idx;

	if (!state)
		return 0;

	for (idx = 0; idx < LOOP_NUM_TYPES; idx++) {
		if (!g_strcmp0(LOOP_STATES[idx], state))
			return idx;
        }

	/* default to 'off' state if invalid */
	return 0;
}

static void mediaplayer_set_role_state(afb_api_t api, int state)
{
	data.playing = (state == GST_STATE_PLAYING);
	gst_element_set_state(data.playbin, state);
}

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

	if (current_track && current_track->data)
		json_object_object_add(jresp, "selected",
			json_object_new_boolean(track == current_track->data));

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

	ret = json_object_object_get_ex(jdict, "type", &val);
	if (!ret) {
		g_free(item->media_path);
		return ret;
	}
	item->media_type = g_strdup(json_object_get_string(val));

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

static int set_media_uri(struct playlist_item *item, int state)
{
	if (!item || !item->media_path)
	{
		AFB_ERROR("Failed to set media URI: no item provided!");
		return -ENOENT;
	}

	gst_element_set_state(data.playbin, GST_STATE_NULL);
	AFB_DEBUG("GSTREAMER playbin.state = GST_STATE_NULL");

	g_object_set(data.playbin, "uri", item->media_path, NULL);
	AFB_DEBUG("GSTREAMER playbin.uri = %s", item->media_path);

	data.position = GST_CLOCK_TIME_NONE;
	data.duration = GST_CLOCK_TIME_NONE;

	if (state) {
		g_object_set(data.playbin, "audio-sink", data.audio_sink, NULL);
		AFB_DEBUG("GSTREAMER playbin.audio-sink = pipewire-sink");

		if (!data.playing)
			mediaplayer_set_role_state(data.api, GST_STATE_PLAYING);
		else
			gst_element_set_state(data.playbin, GST_STATE_PLAYING);

		AFB_DEBUG("GSTREAMER playbin.state = GST_STATE_PLAYING");
	} else {
		g_object_set(data.playbin, "audio-sink", data.fake_sink, NULL);
		AFB_DEBUG("GSTREAMER playbin.audio-sink = fake-sink");

		gst_element_set_state(data.playbin, GST_STATE_PAUSED);
		AFB_DEBUG("GSTREAMER playbin.state = GST_STATE_PAUSED");
	}

	double vol = (double) data.volume / 100.0;
	g_object_set(data.playbin, "volume", vol, NULL);
	AFB_DEBUG("GSTREAMER playbin.volume = %f", vol);

	return 0;
}


static int in_list(gconstpointer item, gconstpointer list)
{
	return g_strcmp0(((struct playlist_item *) item)->media_path,
			 ((struct playlist_item *) list)->media_path);
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
		if (!ret || g_list_find_custom(playlist, item, in_list)) {
			g_free_playlist_item(item);
			continue;
		}

		item->id = idx++;
		playlist = g_list_append(playlist, item);
	}

	if (current_track == NULL) {
		current_track = g_list_first(playlist);
		if (current_track && current_track->data)
			set_media_uri(current_track->data, FALSE);
	}
}

static json_object *populate_json_playlist(json_object *jresp)
{
	GList *l;
	json_object *jarray = json_object_new_array();

	for (l = playlist; l; l = l->next) {
		struct playlist_item *track = l->data;

		if (track && !g_strcmp0(track->media_type, "audio")) {
			json_object *item = populate_json(track);
			json_object_array_add(jarray, item);
		}
	}

	json_object_object_add(jresp, "list", jarray);

	return jresp;
}

static void audio_playlist(afb_req_t request)
{
	const char *value = afb_req_value(request, "list");
	json_object *jresp = NULL;

	g_mutex_lock(&mutex);

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

	g_mutex_unlock(&mutex);
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

static int seek_track(int cmd)
{
	GList *item = NULL;
	int ret;

	if (current_track == NULL)
		return -EINVAL;

	item = (cmd == NEXT_CMD) ? current_track->next : current_track->prev;

	if (item == NULL) {
		if (cmd == PREVIOUS_CMD) {
			seek_stream("0", SEEK_CMD);
			return 0;
		}
		return -EINVAL;
	}

	ret = set_media_uri(item->data, TRUE);
	if (ret < 0)
		return -EINVAL;

	current_track = item;

	return 0;
}

static void avrcp_controls(afb_req_t request)
{
	const char *value = afb_req_value(request, "value");
	const char *action = NULL;
	afb_api_t api = afb_req_get_api(request);
	int cmd, ret;
	json_object *response, *jresp = NULL;

	if (!g_strcmp0(value, "connect") || !g_strcmp0(value, "disconnect")) {
		action = value;
	} else {
		cmd = get_command_index(value);

		if (cmd < 0) {
			afb_req_fail(request, "failed", "unknown command");
			return;
		}

		action = avrcp_control_commands[cmd];
		if (!action) {
			afb_req_fail(request, "failed", "command not supported");
			return;
		}
	}

	jresp = json_object_new_object();
	json_object_object_add(jresp, "action", json_object_new_string(action));

	ret = afb_api_call_sync(api, "Bluetooth-Manager",
			        "avrcp_controls", jresp, &response, NULL, NULL);
	json_object_put(response);

	if (ret < 0) {
		afb_req_fail(request, "failed", "cannot request avrcp_control");
		return;
	}

	afb_req_success(request, NULL, NULL);
}

static void gstreamer_controls(afb_req_t request)
{
	const char *value = afb_req_value(request, "value");
	const char *position = afb_req_value(request, "position");
	int cmd = get_command_index(value);
	afb_api_t api = afb_req_get_api(request);
	json_object *jresp = NULL;

	errno = 0;

	switch (cmd) {
	case PLAY_CMD: {
		GstElement *obj = NULL;
		g_object_get(data.playbin, "audio-sink", &obj, NULL);

		if (obj == data.fake_sink) {
			if (current_track && current_track->data)
				set_media_uri(current_track->data, TRUE);
			else {
				afb_req_fail(request, "failed", "No playlist");
				return;
			}
		} else {
			g_object_set(data.playbin, "audio-sink", data.audio_sink, NULL);
			AFB_DEBUG("GSTREAMER playbin.audio-sink = pipewire-sink");

			mediaplayer_set_role_state(api, GST_STATE_PLAYING);
			AFB_DEBUG("GSTREAMER playbin.state = GST_STATE_PLAYING");
		}

		jresp = json_object_new_object();
		json_object_object_add(jresp, "playing", json_object_new_boolean(TRUE));
		break;
	}
	case PAUSE_CMD:
		mediaplayer_set_role_state(api, GST_STATE_PAUSED);
		AFB_DEBUG("GSTREAMER playbin.state = GST_STATE_PAUSED");
		data.playing = FALSE;

		/* metadata event */
		jresp = populate_json_metadata();
		json_object_object_add(jresp, "status",
				       json_object_new_string("stopped"));
		afb_event_push(metadata_event, jresp);

		/* status returned */
		jresp = json_object_new_object();
		json_object_object_add(jresp, "playing", json_object_new_boolean(FALSE));
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

		if (idx == 0 && errno) {
			afb_req_fail(request, "failed", "invalid index");
			return;
		}

		list = find_media_index(playlist, idx);
		if (list != NULL) {
			struct playlist_item *item = list->data;
			set_media_uri(item, TRUE);
			current_track = list;
		} else {
			afb_req_fail(request, "failed", "couldn't find index");
			return;
		}

		break;
	}
	case VOLUME_CMD: {
		const char *parameter = afb_req_value(request, "volume");
		long int volume;

		if (!parameter) {
			afb_req_fail(request, "failed", "invalid volume");
			return;
		}

		volume = strtol(parameter, NULL, 10);
		errno = 0;

		if (volume == 0 && errno) {
			afb_req_fail(request, "failed", "invalid volume");
			return;
		}

		if (volume < 0)
			volume = 0;
		if (volume > 100)
			volume = 100;

		g_object_set(data.playbin, "volume", (double) volume / 100.0, NULL);
		AFB_DEBUG("GSTREAMER volume = %f", (double) volume / 100.0);

		data.volume = volume;

		break;
	}
	case LOOP_CMD:
		data.loop_state =
			find_loop_state_idx(afb_req_value(request, "state"));
		break;
	case STOP_CMD:
		mediaplayer_set_role_state(api, GST_STATE_NULL);
		AFB_DEBUG("GSTREAMER playbin.state = GST_STATE_NULL");
		break;
	default:
		afb_req_fail(request, "failed", "unknown command");
		return;
	}

	afb_req_success(request, jresp, NULL);
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
 *   loop         - set looping of playlist (true or false)
 */

static void controls(afb_req_t request)
{
	const char *value = afb_req_value(request, "value");

	if (!value) {
		afb_req_fail(request, "failed", "no value was passed");
		return;
	}

	g_mutex_lock(&mutex);
	if (data.avrcp_connected || !g_strcmp0(value, "connect")) {
		g_mutex_unlock(&mutex);
		avrcp_controls(request);
		return;
	}

	gstreamer_controls(request);
	g_mutex_unlock(&mutex);
}

static GstSample *parse_album(GstTagList *tags, gchar *tag_type)
{
	GstSample *sample = NULL;
	int num = gst_tag_list_get_tag_size(tags, tag_type);
	guint i;

	for (i = 0; i < num ; i++) {
		const GValue *value;
		GstStructure *caps;
		int type;

		value = gst_tag_list_get_value_index(tags, tag_type, i);
		if (value == NULL)
			break;

		sample = gst_value_get_sample(value);
		caps = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
		gst_structure_get_enum(caps, "image-type",
				       GST_TYPE_TAG_IMAGE_TYPE, &type);

		if (type == GST_TAG_IMAGE_TYPE_FRONT_COVER)
			break;
	}

	return sample;
}

static gchar *get_album_art(GstTagList *tags)
{
	GstSample *sample;

	sample = parse_album(tags, GST_TAG_IMAGE);
	if (!sample)
		sample = parse_album(tags, GST_TAG_PREVIEW_IMAGE);

	if (sample) {
		GstBuffer *buffer = gst_sample_get_buffer(sample);
		GstMapInfo map;
		gchar *data, *mime_type, *image;

		if (!gst_buffer_map (buffer, &map, GST_MAP_READ))
			return NULL;

		image = g_base64_encode(map.data, map.size);
		mime_type = g_content_type_guess(NULL, map.data, map.size, NULL);

		data = g_strconcat("data:", mime_type, ";base64,", image, NULL);

		g_free(image);
		g_free(mime_type);
		gst_buffer_unmap(buffer, &map);

		return data;
	}

	return NULL;
}

static json_object *populate_json_metadata(void)
{
	struct playlist_item *track;
	json_object *jresp, *metadata;

	if (current_track == NULL || current_track->data == NULL)
		return NULL;

	track = current_track->data;
	metadata = populate_json(track);
	jresp = json_object_new_object();

	if (data.duration != GST_CLOCK_TIME_NONE)
		json_object_object_add(metadata, "duration",
			       json_object_new_int64(data.duration / GST_MSECOND));

	if (data.position != GST_CLOCK_TIME_NONE)
		json_object_object_add(jresp, "position",
			       json_object_new_int64(data.position / GST_MSECOND));

	json_object_object_add(jresp, "volume",
			       json_object_new_int64(data.volume));

	return jresp;
}

static int bluetooth_subscribe(afb_api_t api)
{
	json_object *response, *query;
	int ret;

	query = json_object_new_object();
	json_object_object_add(query, "value", json_object_new_string("media"));

	ret = afb_api_call_sync(api, "Bluetooth-Manager", "subscribe", query, &response, NULL, NULL);
	json_object_put(response);

	if (ret < 0) {
		AFB_ERROR("Cannot subscribe to Bluetooth media event");
		return ret;
	}

	return 0;
}

static void subscribe(afb_req_t request)
{
	const char *value = afb_req_value(request, "value");

	if (!strcasecmp(value, "metadata")) {
		afb_api_t api = afb_req_get_api(request);
		json_object *jresp = NULL;

		afb_req_subscribe(request, metadata_event);
		afb_req_success(request, NULL, NULL);

		g_mutex_lock(&mutex);
		jresp = populate_json_metadata();
		g_mutex_unlock(&mutex);

		afb_event_push(metadata_event, jresp);

		bluetooth_subscribe(api);

		return;
	} else if (!strcasecmp(value, "playlist")) {
		json_object *jresp = json_object_new_object();

		afb_req_subscribe(request, playlist_event);
		afb_req_success(request, NULL, NULL);

		g_mutex_lock(&mutex);
		jresp = populate_json_playlist(jresp);
		g_mutex_unlock(&mutex);

		afb_event_push(playlist_event, jresp);

		return;
	}

	afb_req_fail(request, "failed", "Invalid event");
}

static void unsubscribe(afb_req_t request)
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

		g_mutex_lock(&mutex);

		data->position = GST_CLOCK_TIME_NONE;
		data->duration = GST_CLOCK_TIME_NONE;

		if (data->loop_state == LOOP_TRACK)
			ret = seek_stream("0", SEEK_CMD);
		else
			ret = seek_track(NEXT_CMD);

		if (ret < 0) {
			int loop_playlist = data->loop_state == LOOP_PLAYLIST;

			if (!loop_playlist) {
				mediaplayer_set_role_state(data->api, GST_STATE_NULL);
				data->one_time = TRUE;
			}

			current_track = playlist;

			if (current_track != NULL)
				set_media_uri(current_track->data, loop_playlist);
		}

		g_mutex_unlock(&mutex);
		break;
	}
	case GST_MESSAGE_DURATION:
		data->duration = GST_CLOCK_TIME_NONE;
		break;
	case GST_MESSAGE_TAG: {
		GstTagList *tags = NULL;
		gchar *image = NULL;
		json_object *jresp, *jobj;

		// TODO: This will get triggered multipl times due to gstreamer
		// pipeline, and should be fixed in the future to stop spurious
		// events
		gst_message_parse_tag(msg, &tags);

		if (!tags)
			break;

		image = get_album_art(tags);

		jobj = json_object_new_object();
		json_object_object_add(jobj, "image",
			json_object_new_string(image ? image : ""));

		g_free(image);

		jresp = json_object_new_object();
		json_object_object_add(jresp, "track", jobj);

		afb_event_push(metadata_event, jresp);

		gst_tag_list_unref(tags);

		break;
	}
	default:
		break;
	}

	return TRUE;
}

static gboolean position_event(CustomData *data)
{
	struct playlist_item *track;
	json_object *jresp = NULL, *metadata;

	g_mutex_lock(&mutex);

	if (data->one_time) {
		data->one_time = FALSE;

		json_object *jresp = json_object_new_object();
		json_object_object_add(jresp, "status",
				       json_object_new_string("stopped"));
		g_mutex_unlock(&mutex);

		afb_event_push(metadata_event, jresp);
		return TRUE;
	}

	if (!data->playing || current_track == NULL) {
		g_mutex_unlock(&mutex);
		return TRUE;
	}

	track = current_track->data;
	metadata = populate_json(track);
	jresp = json_object_new_object();

	if (!GST_CLOCK_TIME_IS_VALID(data->duration))
		gst_element_query_duration(data->playbin,
					GST_FORMAT_TIME, &data->duration);

	gst_element_query_position(data->playbin,
					GST_FORMAT_TIME, &data->position);

	json_object_object_add(metadata, "duration",
			       json_object_new_int64(data->duration / GST_MSECOND));
	json_object_object_add(jresp, "position",
			       json_object_new_int64(data->position / GST_MSECOND));
	json_object_object_add(jresp, "status",
			       json_object_new_string("playing"));

	if (metadata_track != current_track)
		metadata_track = current_track;

	json_object_object_add(jresp, "track", metadata);

	g_mutex_unlock(&mutex);

        afb_event_push(metadata_event, jresp);

	return TRUE;
}

static void gstreamer_init(afb_api_t api)
{
	GstBus *bus;
	json_object *response;
	int ret;

	gst_init(NULL, NULL);

	data.api = api;
	data.playbin = gst_element_factory_make("playbin", "playbin");
	if (!data.playbin) {
		AFB_ERROR("GST Pipeline: Failed to create 'playbin' element!");
		exit(1);
	}

	data.fake_sink = gst_element_factory_make("fakesink", NULL);
	if (!data.fake_sink)
	{
		AFB_ERROR("GST Pipeline: Failed to create 'fakesink' element!");
		exit(1);
	}

	data.audio_sink = gst_element_factory_make("pwaudiosink", NULL);
	if (!data.audio_sink)
	{
		AFB_ERROR("GST Pipeline: Failed to create 'pwaudiosink' element!");
		exit(1);
	}
	gst_util_set_object_arg(G_OBJECT(data.audio_sink),
				"stream-properties", "p,media.role=Multimedia");

	g_object_set(data.playbin, "audio-sink", data.fake_sink, NULL);
	AFB_DEBUG("GSTREAMER playbin.audio-sink = fake-sink");

	gst_element_set_state(data.playbin, GST_STATE_PAUSED);
	AFB_DEBUG("GSTREAMER playbin.state = GST_STATE_PAUSED");

	bus = gst_element_get_bus(data.playbin);
	gst_bus_add_watch(bus, (GstBusFunc) handle_message, &data);
	g_timeout_add_seconds(1, (GSourceFunc) position_event, &data);

	ret = afb_api_call_sync(api, "mediascanner", "media_result", NULL, &response, NULL, NULL);
	if (!ret) {
		json_object *val = NULL;
		gboolean ret;

		ret = json_object_object_get_ex(response, "Media", &val);

		if (ret)
			populate_playlist(val);
	}
	json_object_put(response);
}

static void onevent(afb_api_t api, const char *event, struct json_object *object)
{
	json_object *jresp = NULL;

	if (!g_strcmp0(event, "mediascanner/media_added")) {
		json_object *val = NULL;
		gboolean ret;

		g_mutex_lock(&mutex);

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

		g_mutex_lock(&mutex);

		while (l) {
			struct playlist_item *item = l->data;

			l = l->next;

			if (!strncasecmp(path, item->media_path, strlen(path))) {
				if (current_track && current_track->data == item) {
					current_track = NULL;
					data.one_time = TRUE;
					mediaplayer_set_role_state(api, GST_STATE_NULL);
					AFB_DEBUG("GSTREAMER playbin.state = GST_STATE_NULL");
				}

				playlist = g_list_remove(playlist, item);
				g_free_playlist_item(item);
			}
		}

		if (current_track == NULL)
			current_track = g_list_first(playlist);
	} else if (!g_ascii_strcasecmp(event, "Bluetooth-Manager/media")) {
		json_object *val;

		g_mutex_lock(&mutex);

		if (json_object_object_get_ex(object, "connected", &val)) {
			gboolean state = json_object_get_boolean(val);
			data.avrcp_connected = state;

			if (state) {
				mediaplayer_set_role_state(api, GST_STATE_PAUSED);
			} else {
				json_object *jresp = populate_json_metadata();

				if (!jresp)
					jresp = json_object_new_object();

				json_object_object_add(jresp, "status",
				       json_object_new_string("stopped"));
				afb_event_push(metadata_event, jresp);
			}
		}

		g_mutex_unlock(&mutex);

		json_object_get(object);
		afb_event_push(metadata_event, object);

		return;
	} else {
		AFB_ERROR("Invalid event: %s", event);
		return;
	}

	jresp = json_object_new_object();
	jresp = populate_json_playlist(jresp);

	g_mutex_unlock(&mutex);

        afb_event_push(playlist_event, jresp);
}

void *gstreamer_loop_thread(void *ptr)
{
	g_main_loop_run(g_main_loop_new(NULL, FALSE));

	return NULL;
}

static int init(afb_api_t api)
{
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

	ret = afb_api_call_sync(api, "mediascanner", "subscribe", query, &response, NULL, NULL);
	json_object_put(response);

	if (ret < 0) {
		AFB_ERROR("Cannot subscribe to mediascanner media_added event");
		return ret;
	}

	query = json_object_new_object();
	json_object_object_add(query, "value", json_object_new_string("media_removed"));

	ret = afb_api_call_sync(api, "mediascanner", "subscribe", query, &response, NULL, NULL);
	json_object_put(response);

	if (ret < 0) {
		AFB_ERROR("Cannot subscribe to mediascanner media_remove event");
		return ret;
	}

	metadata_event = afb_daemon_make_event("metadata");
	playlist_event = afb_daemon_make_event("playlist");

	gstreamer_init(api);

	return pthread_create(&thread_id, NULL, gstreamer_loop_thread, NULL);
}

static const afb_verb_t binding_verbs[] = {
	{ .verb = "playlist",     .callback = audio_playlist, .info = "Get/set playlist" },
	{ .verb = "controls",     .callback = controls,       .info = "Audio controls" },
	{ .verb = "subscribe",    .callback = subscribe,      .info = "Subscribe to GStreamer events" },
	{ .verb = "unsubscribe",  .callback = unsubscribe,    .info = "Unsubscribe to GStreamer events" },
	{ }
};

/*
 * binder API description
 */
const afb_binding_t afbBindingV3 = {
	.api = "mediaplayer",
	.specification = "Mediaplayer API",
	.verbs = binding_verbs,
	.onevent = onevent,
	.init = init,
};
