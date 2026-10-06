// SPDX-License-Identifier: GPL-2.0-only
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-util.h>
#include <wlr/util/log.h>
#include "common/mem.h"
#include "config/rcxml.h"
#include "output-config.h"

static struct wl_list configs = { &configs, &configs };

static void
free_configs(void)
{
	struct output_config *config, *tmp;
	wl_list_for_each_safe(config, tmp, &configs, link) {
		wl_list_remove(&config->link);
		free(config->name);
		free(config->icc_profile);
		free(config);
	}
}

static int
int_attr(xmlNode *node, const char *name, int fallback)
{
	xmlChar *value = xmlGetProp(node, (const xmlChar *)name);
	int result = value ? atoi((const char *)value) : fallback;
	xmlFree(value);
	return result;
}

static enum wl_output_transform
parse_transform(const char *value)
{
	static const char *names[] = {
		"normal", "90", "180", "270",
		"flipped", "flipped-90", "flipped-180", "flipped-270",
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!strcmp(value, names[i])) {
			return (enum wl_output_transform)i;
		}
	}
	return WL_OUTPUT_TRANSFORM_NORMAL;
}

static void
parse_output(xmlNode *node)
{
	xmlChar *name = xmlGetProp(node, (const xmlChar *)"name");
	if (!name || !name[0]) {
		xmlFree(name);
		return;
	}
	struct output_config *config = znew(*config);
	config->name = xstrdup((const char *)name);
	xmlFree(name);
	config->scale = 1.0;

	for (xmlNode *child = node->children; child; child = child->next) {
		if (child->type != XML_ELEMENT_NODE) {
			continue;
		}
		const char *tag = (const char *)child->name;
		xmlChar *content = xmlNodeGetContent(child);
		const char *text = content ? (const char *)content : "";
		if (!strcmp(tag, "mode")) {
			config->width = int_attr(child, "width", 0);
			config->height = int_attr(child, "height", 0);
			config->refresh = int_attr(child, "refresh", 0);
			config->has_mode = config->width > 0 && config->height > 0;
		} else if (!strcmp(tag, "position")) {
			config->x = int_attr(child, "x", 0);
			config->y = int_attr(child, "y", 0);
			config->has_position = true;
		} else if (!strcmp(tag, "scale")) {
			double scale = atof(text);
			if (scale >= 0.5 && scale <= 4.0) {
				config->scale = scale;
				config->has_scale = true;
			}
		} else if (!strcmp(tag, "transform")) {
			config->transform = parse_transform(text);
			config->has_transform = true;
		} else if (!strcmp(tag, "enabled")) {
			config->enabled = strcmp(text, "no") != 0;
			config->has_enabled = true;
		} else if (!strcmp(tag, "adaptiveSync")) {
			config->adaptive_sync = !strcmp(text, "yes");
			config->has_adaptive_sync = true;
		} else if (!strcmp(tag, "hdr")) {
			config->hdr = !strcmp(text, "yes");
			config->has_hdr = true;
		} else if (!strcmp(tag, "sdrBrightness")) {
			int nits = atoi(text);
			if (nits >= 40 && nits <= 1000) {
				config->sdr_brightness = nits;
			}
		} else if (!strcmp(tag, "iccProfile") && text[0]) {
			free(config->icc_profile);
			config->icc_profile = xstrdup(text);
		}
		xmlFree(content);
	}
	wl_list_insert(configs.prev, &config->link);
}

void
output_config_load(void)
{
	free_configs();
	if (!rc.config_dir) {
		return;
	}
	char path[4096];
	snprintf(path, sizeof(path), "%s/output.xml", rc.config_dir);
	xmlDoc *doc = xmlReadFile(path, NULL, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (!doc) {
		return;
	}
	xmlNode *root = xmlDocGetRootElement(doc);
	if (root && !strcmp((const char *)root->name, "outputs")) {
		for (xmlNode *node = root->children; node; node = node->next) {
			if (node->type == XML_ELEMENT_NODE
					&& !strcmp((const char *)node->name, "output")) {
				parse_output(node);
			}
		}
	}
	xmlFreeDoc(doc);
	wlr_log(WLR_INFO, "loaded %d saved output configs from %s",
		wl_list_length(&configs), path);
}

void
output_config_finish(void)
{
	free_configs();
}

const struct output_config *
output_config_find(const char *name)
{
	if (!name) {
		return NULL;
	}
	struct output_config *config;
	wl_list_for_each(config, &configs, link) {
		if (!strcmp(config->name, name)) {
			return config;
		}
	}
	return NULL;
}
