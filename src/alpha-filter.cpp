#include "alpha-filter.h"
#include "alpha-packer.hpp"
#include "udp-sender.hpp"

#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace {

constexpr uint32_t VIRTUAL_WIDTH = 143;
constexpr uint32_t VIRTUAL_HEIGHT = 84;
constexpr uint32_t LED_WIDTH = ALPHA_LED_COLS;
constexpr uint32_t LED_HEIGHT = ALPHA_LED_ROWS;

constexpr const char *SETTING_HOST = "host";
constexpr const char *SETTING_PORT = "port";
constexpr const char *SETTING_MAX_FPS = "max_fps";
constexpr const char *SETTING_PREVIEW_OUTPUT = "preview_output";
constexpr const char *SETTING_BLANK_ON_DISABLE = "blank_on_disable";
constexpr const char *SETTING_COLOR_OFF = "color_off";
constexpr const char *SETTING_COLOR_RED = "color_red";
constexpr const char *SETTING_COLOR_GREEN = "color_green";
constexpr const char *SETTING_COLOR_AMBER = "color_amber";

constexpr uint32_t DEFAULT_COLOR_OFF = 0xFF000000U;
constexpr uint32_t DEFAULT_COLOR_RED = 0xFF0000FFU;
constexpr uint32_t DEFAULT_COLOR_GREEN = 0xFF00FF00U;
constexpr uint32_t DEFAULT_COLOR_AMBER = 0xFFFFFFFFU;

struct AlphaFilterData {
	obs_source_t *source = nullptr;

	gs_texrender_t *texrender_src = nullptr;
	gs_texrender_t *texrender_virtual = nullptr;
	gs_texrender_t *texrender_leds = nullptr;

	gs_stagesurf_t *staging[2] = {nullptr, nullptr};
	int write_idx = 0;
	bool has_staged = false;

	gs_samplerstate_t *point_sampler = nullptr;

	gs_effect_t *quantize_effect = nullptr;
	gs_eparam_t *param_image = nullptr;
	gs_eparam_t *param_color_off = nullptr;
	gs_eparam_t *param_color_red = nullptr;
	gs_eparam_t *param_color_green = nullptr;
	gs_eparam_t *param_color_amber = nullptr;

	std::atomic<int> max_fps{0};
	std::atomic<bool> preview_output{false};
	std::atomic<bool> blank_on_disable{true};
	std::atomic<uint32_t> color_off_abgr{DEFAULT_COLOR_OFF};
	std::atomic<uint32_t> color_red_abgr{DEFAULT_COLOR_RED};
	std::atomic<uint32_t> color_green_abgr{DEFAULT_COLOR_GREEN};
	std::atomic<uint32_t> color_amber_abgr{DEFAULT_COLOR_AMBER};

	uint64_t next_stage_ts_ns = 0;
	bool was_enabled = true;
	bool rendered_this_tick = false;

	UdpSender sender;
};

inline struct vec4 abgr_to_vec4(uint32_t abgr)
{
	struct vec4 out;
	const float r = static_cast<float>(abgr & 0xFFU) / 255.0f;
	const float g = static_cast<float>((abgr >> 8) & 0xFFU) / 255.0f;
	const float b = static_cast<float>((abgr >> 16) & 0xFFU) / 255.0f;
	vec4_set(&out, r, g, b, 1.0f);
	return out;
}

const char *locale_text(const char *key, const char *fallback)
{
	const char *translated = nullptr;
	if (obs_module_get_string(key, &translated) && translated)
		return translated;
	return fallback;
}

const char *alpha_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return locale_text("AlphaDirectorOutput", "Alpha Director Output");
}

void alpha_filter_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_HOST, "espressif.local");
	obs_data_set_default_int(settings, SETTING_PORT, 5005);
	obs_data_set_default_int(settings, SETTING_MAX_FPS, 0);
	obs_data_set_default_bool(settings, SETTING_PREVIEW_OUTPUT, false);
	obs_data_set_default_bool(settings, SETTING_BLANK_ON_DISABLE, true);
	obs_data_set_default_int(settings, SETTING_COLOR_OFF, static_cast<long long>(DEFAULT_COLOR_OFF));
	obs_data_set_default_int(settings, SETTING_COLOR_RED, static_cast<long long>(DEFAULT_COLOR_RED));
	obs_data_set_default_int(settings, SETTING_COLOR_GREEN, static_cast<long long>(DEFAULT_COLOR_GREEN));
	obs_data_set_default_int(settings, SETTING_COLOR_AMBER, static_cast<long long>(DEFAULT_COLOR_AMBER));
}

obs_properties_t *alpha_filter_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, SETTING_HOST, locale_text("Host", "ESP32 Host / IPv4"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, SETTING_PORT, locale_text("Port", "UDP Port"), 1, 65535, 1);
	obs_properties_add_int(props, SETTING_MAX_FPS, locale_text("MaxFps", "Max Transmit FPS (0 = Canvas)"), 0, 120,
			       1);
	obs_properties_add_bool(props, SETTING_PREVIEW_OUTPUT,
				locale_text("PreviewOutput", "Preview Quantized Output"));
	obs_properties_add_bool(props, SETTING_BLANK_ON_DISABLE,
				locale_text("BlankOnDisable", "Blank Matrix on Disable"));

	obs_properties_add_color(props, SETTING_COLOR_OFF, locale_text("ColorOff", "Palette: Off (0,0)"));
	obs_properties_add_color(props, SETTING_COLOR_RED, locale_text("ColorRed", "Palette: Red (1,0)"));
	obs_properties_add_color(props, SETTING_COLOR_GREEN, locale_text("ColorGreen", "Palette: Green (0,1)"));
	obs_properties_add_color(props, SETTING_COLOR_AMBER, locale_text("ColorAmber", "Palette: Amber (1,1)"));

	return props;
}

void alpha_filter_update(void *data, obs_data_t *settings)
{
	auto *filter = static_cast<AlphaFilterData *>(data);

	const char *host = obs_data_get_string(settings, SETTING_HOST);
	const int port = std::clamp(static_cast<int>(obs_data_get_int(settings, SETTING_PORT)), 1, 65535);
	const int max_fps = std::clamp(static_cast<int>(obs_data_get_int(settings, SETTING_MAX_FPS)), 0, 120);

	filter->max_fps.store(max_fps, std::memory_order_relaxed);
	filter->preview_output.store(obs_data_get_bool(settings, SETTING_PREVIEW_OUTPUT), std::memory_order_relaxed);
	filter->blank_on_disable.store(obs_data_get_bool(settings, SETTING_BLANK_ON_DISABLE), std::memory_order_relaxed);

	filter->color_off_abgr.store(static_cast<uint32_t>(obs_data_get_int(settings, SETTING_COLOR_OFF)),
				     std::memory_order_relaxed);
	filter->color_red_abgr.store(static_cast<uint32_t>(obs_data_get_int(settings, SETTING_COLOR_RED)),
				     std::memory_order_relaxed);
	filter->color_green_abgr.store(static_cast<uint32_t>(obs_data_get_int(settings, SETTING_COLOR_GREEN)),
				       std::memory_order_relaxed);
	filter->color_amber_abgr.store(static_cast<uint32_t>(obs_data_get_int(settings, SETTING_COLOR_AMBER)),
				       std::memory_order_relaxed);

	filter->next_stage_ts_ns = 0;
	filter->sender.set_endpoint(host ? host : "", static_cast<uint16_t>(port));
}

void destroy_graphics_resources(AlphaFilterData *filter)
{
	obs_enter_graphics();

	if (filter->quantize_effect) {
		gs_effect_destroy(filter->quantize_effect);
		filter->quantize_effect = nullptr;
	}
	if (filter->point_sampler) {
		gs_samplerstate_destroy(filter->point_sampler);
		filter->point_sampler = nullptr;
	}
	for (int i = 0; i < 2; ++i) {
		if (filter->staging[i]) {
			gs_stagesurface_destroy(filter->staging[i]);
			filter->staging[i] = nullptr;
		}
	}
	if (filter->texrender_leds) {
		gs_texrender_destroy(filter->texrender_leds);
		filter->texrender_leds = nullptr;
	}
	if (filter->texrender_virtual) {
		gs_texrender_destroy(filter->texrender_virtual);
		filter->texrender_virtual = nullptr;
	}
	if (filter->texrender_src) {
		gs_texrender_destroy(filter->texrender_src);
		filter->texrender_src = nullptr;
	}

	obs_leave_graphics();
}

void *alpha_filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new AlphaFilterData();
	filter->source = source;
	filter->was_enabled = obs_source_enabled(source);

	char *effect_path = obs_module_file("alpha-quantize.effect");
	char *effect_errors = nullptr;

	obs_enter_graphics();

	filter->texrender_src = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	filter->texrender_virtual = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	filter->texrender_leds = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	filter->staging[0] = gs_stagesurface_create(LED_WIDTH, LED_HEIGHT, GS_RGBA);
	filter->staging[1] = gs_stagesurface_create(LED_WIDTH, LED_HEIGHT, GS_RGBA);

	struct gs_sampler_info sampler_info{};
	sampler_info.filter = GS_FILTER_POINT;
	sampler_info.address_u = GS_ADDRESS_CLAMP;
	sampler_info.address_v = GS_ADDRESS_CLAMP;
	sampler_info.address_w = GS_ADDRESS_CLAMP;
	sampler_info.max_anisotropy = 1;
	filter->point_sampler = gs_samplerstate_create(&sampler_info);

	if (effect_path) {
		filter->quantize_effect = gs_effect_create_from_file(effect_path, &effect_errors);
		if (filter->quantize_effect) {
			filter->param_image = gs_effect_get_param_by_name(filter->quantize_effect, "image");
			filter->param_color_off = gs_effect_get_param_by_name(filter->quantize_effect, "color_off");
			filter->param_color_red = gs_effect_get_param_by_name(filter->quantize_effect, "color_red");
			filter->param_color_green = gs_effect_get_param_by_name(filter->quantize_effect, "color_green");
			filter->param_color_amber = gs_effect_get_param_by_name(filter->quantize_effect, "color_amber");
		}
	}

	obs_leave_graphics();

	if (!filter->quantize_effect) {
		obs_log(LOG_ERROR, "Failed to load alpha-quantize.effect (%s): %s",
			effect_path ? effect_path : "<missing>", effect_errors ? effect_errors : "unknown error");
		bfree(effect_errors);
		bfree(effect_path);
		filter->sender.shutdown(false);
		destroy_graphics_resources(filter);
		delete filter;
		return nullptr;
	}

	bfree(effect_errors);
	bfree(effect_path);

	alpha_filter_update(filter, settings);
	return filter;
}

void alpha_filter_destroy(void *data)
{
	auto *filter = static_cast<AlphaFilterData *>(data);
	if (!filter)
		return;

	filter->sender.shutdown(filter->blank_on_disable.load(std::memory_order_relaxed));
	destroy_graphics_resources(filter);
	delete filter;
}

void handle_disable_blank(AlphaFilterData *filter)
{
	filter->has_staged = false;
	if (filter->blank_on_disable.load(std::memory_order_relaxed)) {
		filter->sender.submit_blank_frame();
	}
}

void alpha_filter_deactivate(void *data)
{
	handle_disable_blank(static_cast<AlphaFilterData *>(data));
}

void alpha_filter_hide(void *data)
{
	handle_disable_blank(static_cast<AlphaFilterData *>(data));
}

void alpha_filter_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	auto *filter = static_cast<AlphaFilterData *>(data);

	filter->rendered_this_tick = false;

	const bool enabled = obs_source_enabled(filter->source);
	if (filter->was_enabled && !enabled) {
		handle_disable_blank(filter);
	}
	filter->was_enabled = enabled;
}

bool should_stage_frame(AlphaFilterData *filter)
{
	const int fps_limit = filter->max_fps.load(std::memory_order_relaxed);
	if (fps_limit <= 0)
		return true;

	uint64_t now_ns = obs_get_video_frame_time();
	if (now_ns == 0)
		now_ns = os_gettime_ns();

	const uint64_t interval_ns = 1000000000ULL / static_cast<uint64_t>(fps_limit);
	if (now_ns < filter->next_stage_ts_ns)
		return false;

	filter->next_stage_ts_ns = now_ns + interval_ns;
	return true;
}

void render_source_to_texrender(AlphaFilterData *filter, obs_source_t *target, obs_source_t *parent, uint32_t cx,
				uint32_t cy)
{
	gs_texrender_reset(filter->texrender_src);
	if (!gs_texrender_begin(filter->texrender_src, cx, cy))
		return;

	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, static_cast<float>(cx), 0.0f, static_cast<float>(cy), -100.0f, 100.0f);

	const uint32_t target_flags = obs_source_get_output_flags(target);
	const bool custom_draw = (target_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	const bool async = (target_flags & OBS_SOURCE_ASYNC) != 0;
	if (target == parent && !custom_draw && !async)
		obs_source_default_render(target);
	else
		obs_source_video_render(target);

	gs_blend_state_pop();
	gs_texrender_end(filter->texrender_src);
}

void downscale_to_virtual(AlphaFilterData *filter, gs_texture_t *src_tex, uint32_t src_cx, uint32_t src_cy)
{
	if (!src_tex)
		return;

	gs_effect_t *scale_effect = obs_get_base_effect(OBS_EFFECT_BICUBIC);
	if (!scale_effect)
		scale_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (!scale_effect)
		return;

	gs_texrender_reset(filter->texrender_virtual);
	if (!gs_texrender_begin(filter->texrender_virtual, VIRTUAL_WIDTH, VIRTUAL_HEIGHT))
		return;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, static_cast<float>(VIRTUAL_WIDTH), 0.0f, static_cast<float>(VIRTUAL_HEIGHT), -100.0f, 100.0f);

	gs_eparam_t *image_param = gs_effect_get_param_by_name(scale_effect, "image");
	gs_eparam_t *dim_param = gs_effect_get_param_by_name(scale_effect, "base_dimension");
	gs_eparam_t *dim_i_param = gs_effect_get_param_by_name(scale_effect, "base_dimension_i");

	if (image_param)
		gs_effect_set_texture(image_param, src_tex);
	if (dim_param) {
		struct vec2 dim;
		vec2_set(&dim, static_cast<float>(src_cx), static_cast<float>(src_cy));
		gs_effect_set_vec2(dim_param, &dim);
	}
	if (dim_i_param) {
		struct vec2 dim_i;
		vec2_set(&dim_i, 1.0f / static_cast<float>(src_cx), 1.0f / static_cast<float>(src_cy));
		gs_effect_set_vec2(dim_i_param, &dim_i);
	}

	while (gs_effect_loop(scale_effect, "Draw"))
		gs_draw_sprite(src_tex, 0, VIRTUAL_WIDTH, VIRTUAL_HEIGHT);

	gs_blend_state_pop();
	gs_texrender_end(filter->texrender_virtual);
}

void quantize_to_leds(AlphaFilterData *filter, gs_texture_t *virtual_tex)
{
	if (!virtual_tex || !filter->quantize_effect)
		return;

	gs_texrender_reset(filter->texrender_leds);
	if (!gs_texrender_begin(filter->texrender_leds, LED_WIDTH, LED_HEIGHT))
		return;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, static_cast<float>(LED_WIDTH), 0.0f, static_cast<float>(LED_HEIGHT), -100.0f, 100.0f);

	const struct vec4 c_off = abgr_to_vec4(filter->color_off_abgr.load(std::memory_order_relaxed));
	const struct vec4 c_red = abgr_to_vec4(filter->color_red_abgr.load(std::memory_order_relaxed));
	const struct vec4 c_green = abgr_to_vec4(filter->color_green_abgr.load(std::memory_order_relaxed));
	const struct vec4 c_amber = abgr_to_vec4(filter->color_amber_abgr.load(std::memory_order_relaxed));

	if (filter->param_image)
		gs_effect_set_texture(filter->param_image, virtual_tex);
	if (filter->param_color_off)
		gs_effect_set_vec4(filter->param_color_off, &c_off);
	if (filter->param_color_red)
		gs_effect_set_vec4(filter->param_color_red, &c_red);
	if (filter->param_color_green)
		gs_effect_set_vec4(filter->param_color_green, &c_green);
	if (filter->param_color_amber)
		gs_effect_set_vec4(filter->param_color_amber, &c_amber);

	while (gs_effect_loop(filter->quantize_effect, "Draw"))
		gs_draw_sprite(virtual_tex, 0, LED_WIDTH, LED_HEIGHT);

	gs_blend_state_pop();
	gs_texrender_end(filter->texrender_leds);
}

void draw_output_texture(AlphaFilterData *filter, gs_texture_t *out_tex, uint32_t cx, uint32_t cy, bool point_sample)
{
	if (!out_tex)
		return;

	gs_effect_t *def_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (!def_effect)
		return;

	gs_eparam_t *image_param = gs_effect_get_param_by_name(def_effect, "image");
	if (image_param) {
		gs_effect_set_texture(image_param, out_tex);
		if (point_sample && filter->point_sampler)
			gs_effect_set_next_sampler(image_param, filter->point_sampler);
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(def_effect, "Draw"))
		gs_draw_sprite(out_tex, 0, cx, cy);

	gs_blend_state_pop();
}

void alpha_filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	auto *filter = static_cast<AlphaFilterData *>(data);

	obs_source_t *target = obs_filter_get_target(filter->source);
	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (!target || !parent || !filter->quantize_effect) {
		obs_source_skip_video_filter(filter->source);
		return;
	}

	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);
	if (cx == 0 || cy == 0) {
		obs_source_skip_video_filter(filter->source);
		return;
	}

	if (!filter->rendered_this_tick) {
		render_source_to_texrender(filter, target, parent, cx, cy);

		gs_texture_t *src_tex = gs_texrender_get_texture(filter->texrender_src);
		downscale_to_virtual(filter, src_tex, cx, cy);

		gs_texture_t *virtual_tex = gs_texrender_get_texture(filter->texrender_virtual);
		quantize_to_leds(filter, virtual_tex);

		gs_texture_t *leds_tex = gs_texrender_get_texture(filter->texrender_leds);
		if (leds_tex && filter->staging[0] && filter->staging[1] && should_stage_frame(filter)) {
			gs_stage_texture(filter->staging[filter->write_idx], leds_tex);

			if (filter->has_staged) {
				const int read_idx = 1 - filter->write_idx;
				uint8_t *mapped_data = nullptr;
				uint32_t mapped_linesize = 0;
				if (gs_stagesurface_map(filter->staging[read_idx], &mapped_data, &mapped_linesize)) {
					const AlphaFrame packed = pack_alpha_frame(mapped_data, mapped_linesize);
					gs_stagesurface_unmap(filter->staging[read_idx]);
					filter->sender.submit_frame(packed);
				}
			}

			filter->write_idx ^= 1;
			filter->has_staged = true;
		}

		filter->rendered_this_tick = true;
	}

	const bool preview = filter->preview_output.load(std::memory_order_relaxed);
	gs_texture_t *out_tex = preview ? gs_texrender_get_texture(filter->texrender_leds)
					: gs_texrender_get_texture(filter->texrender_src);
	draw_output_texture(filter, out_tex, cx, cy, preview);
}

struct obs_source_info build_alpha_filter_info()
{
	struct obs_source_info info{};
	info.id = "alpha_director_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = alpha_filter_get_name;
	info.create = alpha_filter_create;
	info.destroy = alpha_filter_destroy;
	info.get_defaults = alpha_filter_get_defaults;
	info.get_properties = alpha_filter_get_properties;
	info.update = alpha_filter_update;
	info.deactivate = alpha_filter_deactivate;
	info.hide = alpha_filter_hide;
	info.video_tick = alpha_filter_video_tick;
	info.video_render = alpha_filter_video_render;
	return info;
}

} // namespace

void register_alpha_director_filter(void)
{
	static const struct obs_source_info alpha_filter_info = build_alpha_filter_info();
	obs_register_source(&alpha_filter_info);
}

