#include "3d-effect.h"
#include "version.h"
#include <obs-module.h>
#include "graphics/math-defs.h"
#include "graphics/matrix4.h"

struct effect_3d {
	obs_source_t *source;
	float fov;
	struct vec3 position;
	struct vec3 rotation;
	struct vec2 scale;
	struct vec2 shear;

	bool processed_frame;
	gs_texrender_t *render;
	enum gs_color_space space;
};

static const char *effect_3d_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("3DEffect");
}

void effect_3d_update(void *data, obs_data_t *settings)
{
	struct effect_3d *context = data;
	context->fov = (float)obs_data_get_double(settings, "fov");
	context->rotation.x = (float)obs_data_get_double(settings, "rot_x");
	context->rotation.y = (float)obs_data_get_double(settings, "rot_y");
	context->rotation.z = (float)obs_data_get_double(settings, "rot_z");

	context->position.x = (float)obs_data_get_double(settings, "pos_x");
	context->position.y = (float)obs_data_get_double(settings, "pos_y");
	context->position.z = (float)obs_data_get_double(settings, "pos_z");

	context->scale.x =
		(float)obs_data_get_double(settings, "scale_x") / 100.0f;
	context->scale.y =
		(float)obs_data_get_double(settings, "scale_y") / 100.0f;
}

static void *effect_3d_create(obs_data_t *settings, obs_source_t *source)
{
	struct effect_3d *context = bzalloc(sizeof(struct effect_3d));
	context->source = source;
	effect_3d_update(context, settings);
	return context;
}

static void effect_3d_destroy(void *data)
{
	struct effect_3d *context = data;
	bfree(context);
}

static obs_properties_t *effect_3d_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *ppts = obs_properties_create();

	obs_property_t *p = obs_properties_add_float_slider(
		ppts, "fov", obs_module_text("FieldOfView"), 0.1, 180.0, 0.01);

	obs_properties_t *rot = obs_properties_create();

	p = obs_properties_add_float_slider(
		rot, "rot_x", obs_module_text("RotX"), -180.0, 180.0, 0.01);
	obs_property_float_set_suffix(p, "° Deg");

	p = obs_properties_add_float_slider(
		rot, "rot_y", obs_module_text("RotY"), -180.0, 180.0, 0.01);
	obs_property_float_set_suffix(p, "° Deg");

	p = obs_properties_add_float_slider(
		rot, "rot_z", obs_module_text("RotZ"), -180.0, 180.0, 0.01);
	obs_property_float_set_suffix(p, "° Deg");

	obs_properties_add_group(ppts, "rot", obs_module_text("Rotation"),
				 OBS_GROUP_NORMAL, rot);

	obs_properties_t *pos = obs_properties_create();
	p = obs_properties_add_float(pos, "pos_x", obs_module_text("PosX"),
				     -100000.0, 100000.0, 1.0);
	obs_property_float_set_suffix(p, " px");
	p = obs_properties_add_float(pos, "pos_y", obs_module_text("PosY"),
				     -100000.0, 100000.0, 1.0);
	obs_property_float_set_suffix(p, " px");
	p = obs_properties_add_float(pos, "pos_z", obs_module_text("PosZ"),
				     -100000.0, 100000.0, 1.0);
	obs_property_float_set_suffix(p, " px");

	obs_properties_add_group(ppts, "pos", obs_module_text("Position"),
				 OBS_GROUP_NORMAL, pos);

	obs_properties_t *scale = obs_properties_create();

	p = obs_properties_add_float(scale, "scale_x",
				     obs_module_text("ScaleX"), 0.0, 10000.0,
				     1.0f);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float(scale, "scale_y",
				     obs_module_text("ScaleY"), 0.0, 10000.0,
				     1.0f);
	obs_property_float_set_suffix(p, "%");

	obs_properties_add_group(ppts, "scale", obs_module_text("Scale"),
				 OBS_GROUP_NORMAL, scale);

	obs_properties_add_text(
		ppts, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/3d-effect.1692/\">3D Effect</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);
	return ppts;
}

static const char *
get_tech_name_and_multiplier(enum gs_color_space current_space,
			     enum gs_color_space source_space,
			     float *multiplier)
{
	const char *tech_name = "Draw";
	*multiplier = 1.f;

	switch (source_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_EXTENDED:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawTonemap";
			break;
		case GS_CS_709_SCRGB:
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
			break;
		default:
			break;
		}
		break;
	case GS_CS_709_SCRGB:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		default:
			break;
		}
	}

	return tech_name;
}

void effect_3d_draw_frame(struct effect_3d *context, uint32_t w, uint32_t h)
{
	const enum gs_color_space current_space = gs_get_color_space();
	float multiplier;
	const char *technique = get_tech_name_and_multiplier(
		current_space, context->space, &multiplier);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(context->render);
	if (!tex)
		return;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"),
				   tex);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"),
			    multiplier);

	while (gs_effect_loop(effect, technique))
		gs_draw_sprite(tex, 0, w, h);

	gs_enable_framebuffer_srgb(previous);
	gs_blend_state_pop();
}

void effect_3d_video_render(void *data, gs_effect_t *eff)
{
	UNUSED_PARAMETER(eff);
	struct effect_3d *context = data;

	obs_source_t *target = obs_filter_get_target(context->source);
	obs_source_t *parent = obs_filter_get_parent(context->source);
	uint32_t base_width = obs_source_get_base_width(target);
	uint32_t base_height = obs_source_get_base_height(target);
	if (!base_width || !base_height || !target || !parent) {
		obs_source_skip_video_filter(context->source);
		return;
	}

	if (context->processed_frame) {
		effect_3d_draw_frame(context, base_width, base_height);
		return;
	}
	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space space = obs_source_get_color_space(
		target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	const enum gs_color_format format = gs_get_format_from_space(space);
	if (!context->render ||
	    gs_texrender_get_format(context->render) != format) {
		gs_texrender_destroy(context->render);
		context->render = gs_texrender_create(format, GS_ZS_NONE);
	} else {
		gs_texrender_reset(context->render);
	}

	gs_viewport_push();
	gs_matrix_push();
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin_with_color_space(context->render, base_width,
						base_height, space)) {
		const float w = (float)base_width;
		const float h = (float)base_height;
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_perspective(context->fov, w / h, 1.0f / (float)(1 << 22),
			       (float)(1 << 22));
		gs_matrix_translate3f(0.0f, 0.0f, -1.0f);
		gs_matrix_scale3f(w / h, 1.0f, 1.0f);
		gs_matrix_scale3f(context->scale.x, context->scale.y, 1.0f);
		gs_matrix_translate3f(context->position.x / w * 2.0f,
				      context->position.y / h * 2.0f,
				      context->position.z / (w + h));
		gs_matrix_scale3f(h / w, 1.0f, 1.0f);
		gs_matrix_rotaa4f(1.0f, 0.0f, 0.0f, RAD(context->rotation.x));
		gs_matrix_rotaa4f(0.0f, 1.0f, 0.0f, RAD(context->rotation.y));
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(context->rotation.z));
		gs_matrix_scale3f(w / h, 1.0f, 1.0f);
		gs_matrix_translate3f(-1.0f, -1.0f, 0.0f);
		gs_matrix_scale3f(2.0f / w, 2.0f / h, 1.0f);
		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);
		gs_texrender_end(context->render);
		context->space = space;
	}
	gs_blend_state_pop();
	gs_matrix_pop();
	gs_viewport_pop();
	context->processed_frame = true;
	effect_3d_draw_frame(context, base_width, base_height);
}

void effect_3d_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct effect_3d *context = data;
	context->processed_frame = false;
}

void effect_3d_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "fov", 90.0);
	obs_data_set_default_double(settings, "scale_x", 100.0);
	obs_data_set_default_double(settings, "scale_y", 100.0);
}

enum gs_color_space
effect_3d_color_space(void *data, size_t count,
		      const enum gs_color_space *preferred_spaces)
{
	struct effect_3d *context = data;
	obs_source_t *target = obs_filter_get_target(context->source);
	obs_source_t *parent = obs_filter_get_parent(context->source);

	if (!target || !parent) {
		return (count > 0) ? preferred_spaces[0] : GS_CS_SRGB;
	}

	enum gs_color_space space = context->space;
	for (size_t i = 0; i < count; ++i) {
		space = preferred_spaces[i];
		if (space == context->space)
			break;
	}

	return space;
}

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("3d-effect", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("3DEffect");
}

struct obs_source_info effect_3d_info = {
	.id = "3d_effect_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = effect_3d_get_name,
	.create = effect_3d_create,
	.destroy = effect_3d_destroy,
	.get_properties = effect_3d_properties,
	.get_defaults = effect_3d_defaults,
	.video_render = effect_3d_video_render,
	.video_tick = effect_3d_video_tick,
	.update = effect_3d_update,
	.load = effect_3d_update,
	.video_get_color_space = effect_3d_color_space,
};

bool obs_module_load(void)
{
	blog(LOG_INFO, "[3D Effect] loaded version %s", PROJECT_VERSION);
	obs_register_source(&effect_3d_info);
	return true;
}
