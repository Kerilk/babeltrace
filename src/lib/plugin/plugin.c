/*
 * Copyright 2017-2018 Philippe Proulx <pproulx@efficios.com>
 * Copyright 2016 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * Author: Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define BT_LOG_TAG "LIB/PLUGIN"
#include "lib/logging.h"

#include "common/assert.h"
#include "lib/assert-pre.h"
#include "common/macros.h"
#include "compat/compiler.h"
#include "common/common.h"
#include <babeltrace2/plugin/plugin-const.h>
#include <babeltrace2/graph/component-class-const.h>
#include "lib/graph/component-class.h"
#include <babeltrace2/types.h>
#include <glib.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <ftw.h>
#include <pthread.h>

#include "plugin.h"
#include "plugin-so.h"

#define PYTHON_PLUGIN_PROVIDER_FILENAME	"libbabeltrace2-python-plugin-provider." G_MODULE_SUFFIX
#define PYTHON_PLUGIN_PROVIDER_SYM_NAME	bt_plugin_python_create_all_from_file
#define PYTHON_PLUGIN_PROVIDER_SYM_NAME_STR	G_STRINGIFY(PYTHON_PLUGIN_PROVIDER_SYM_NAME)

#define APPEND_ALL_FROM_DIR_NFDOPEN_MAX	8

#ifdef BT_BUILT_IN_PYTHON_PLUGIN_SUPPORT
#include <plugin/python-plugin-provider.h>

static
enum bt_plugin_status (*bt_plugin_python_create_all_from_file_sym)(
		const char *path, bool fail_on_load_error,
		struct bt_plugin_set **plugin_set_out) =
	bt_plugin_python_create_all_from_file;

static
void init_python_plugin_provider(void)
{
}
#else /* BT_BUILT_IN_PYTHON_PLUGIN_SUPPORT */
static GModule *python_plugin_provider_module;

static
enum bt_plugin_status (*bt_plugin_python_create_all_from_file_sym)(
		 const char *path, bool fail_on_load_error,
		 struct bt_plugin_set **plugin_set_out);

static
void init_python_plugin_provider(void) {
	if (bt_plugin_python_create_all_from_file_sym != NULL) {
		return;
	}

	BT_LOGI_STR("Loading Python plugin provider module.");
	python_plugin_provider_module =
		g_module_open(PYTHON_PLUGIN_PROVIDER_FILENAME, 0);
	if (!python_plugin_provider_module) {
		/*
		 * This is not an error. The whole point of having an
		 * external Python plugin provider is that it can be
		 * missing and the Babeltrace library still works.
		 */
		BT_LOGI("Cannot open `%s`: %s: continuing without Python plugin support.",
			PYTHON_PLUGIN_PROVIDER_FILENAME, g_module_error());
		return;
	}

	if (!g_module_symbol(python_plugin_provider_module,
			PYTHON_PLUGIN_PROVIDER_SYM_NAME_STR,
			(gpointer) &bt_plugin_python_create_all_from_file_sym)) {
		/*
		 * This is an error because, since we found the Python
		 * plugin provider shared object, we expect this symbol
		 * to exist.
		 */
		BT_LOGE("Cannot find the Python plugin provider loading symbol: "
			"%s: continuing without Python plugin support: "
			"file=\"%s\", symbol=\"%s\"",
			g_module_error(),
			PYTHON_PLUGIN_PROVIDER_FILENAME,
			PYTHON_PLUGIN_PROVIDER_SYM_NAME_STR);
		return;
	}

	BT_LOGI("Loaded Python plugin provider module: addr=%p",
		python_plugin_provider_module);
}

__attribute__((destructor)) static
void fini_python_plugin_provider(void) {
	if (python_plugin_provider_module) {
		BT_LOGI("Unloading Python plugin provider module.");

		if (!g_module_close(python_plugin_provider_module)) {
			BT_LOGE("Failed to close the Python plugin provider module: %s.",
				g_module_error());
		}

		python_plugin_provider_module = NULL;
	}
}
#endif

uint64_t bt_plugin_set_get_plugin_count(struct bt_plugin_set *plugin_set)
{
	BT_ASSERT_PRE_NON_NULL(plugin_set, "Plugin set");
	return (uint64_t) plugin_set->plugins->len;
}

const struct bt_plugin *bt_plugin_set_borrow_plugin_by_index_const(
		const struct bt_plugin_set *plugin_set, uint64_t index)
{
	BT_ASSERT_PRE_NON_NULL(plugin_set, "Plugin set");
	BT_ASSERT_PRE_VALID_INDEX(index, plugin_set->plugins->len);
	return g_ptr_array_index(plugin_set->plugins, index);
}

enum bt_plugin_status bt_plugin_find_all_from_static(
		bt_bool fail_on_load_error,
		const struct bt_plugin_set **plugin_set_out)
{
	/* bt_plugin_so_create_all_from_static() logs errors */
	return bt_plugin_so_create_all_from_static(fail_on_load_error,
		(void *) plugin_set_out);
}

enum bt_plugin_status bt_plugin_find_all_from_file(const char *path,
		bt_bool fail_on_load_error,
		const struct bt_plugin_set **plugin_set_out)
{
	enum bt_plugin_status status;

	BT_ASSERT_PRE_NON_NULL(path, "Path");
	BT_ASSERT_PRE_NON_NULL(path, "Plugin set (output)");
	BT_LOGI("Creating plugins from file: path=\"%s\"", path);

	/* Try shared object plugins */
	status = bt_plugin_so_create_all_from_file(path, fail_on_load_error,
		(void *) plugin_set_out);
	if (status == BT_PLUGIN_STATUS_OK) {
		BT_ASSERT(*plugin_set_out);
		BT_ASSERT((*plugin_set_out)->plugins->len > 0);
		goto end;
	} else if (status < 0) {
		BT_ASSERT(!*plugin_set_out);
		goto end;
	}

	BT_ASSERT(status == BT_PLUGIN_STATUS_NOT_FOUND);
	BT_ASSERT(!*plugin_set_out);

	/* Try Python plugins if support is available */
	init_python_plugin_provider();
	if (bt_plugin_python_create_all_from_file_sym) {
		status = bt_plugin_python_create_all_from_file_sym(path,
			fail_on_load_error, (void *) plugin_set_out);
		if (status == BT_PLUGIN_STATUS_OK) {
			BT_ASSERT(*plugin_set_out);
			BT_ASSERT((*plugin_set_out)->plugins->len > 0);
			goto end;
		} else if (status < 0) {
			BT_ASSERT(!*plugin_set_out);
			goto end;
		}

		BT_ASSERT(status == BT_PLUGIN_STATUS_NOT_FOUND);
		BT_ASSERT(!*plugin_set_out);
	}

end:
	if (status == BT_PLUGIN_STATUS_OK) {
		BT_LOGI("Created %u plugins from file: "
			"path=\"%s\", count=%u, plugin-set-addr=%p",
			(*plugin_set_out)->plugins->len, path,
			(*plugin_set_out)->plugins->len,
			*plugin_set_out);
	} else if (status == BT_PLUGIN_STATUS_NOT_FOUND) {
		BT_LOGI("Found no plugins in file: path=\"%s\"", path);
	}

	return status;
}

static void destroy_gstring(void *data)
{
	g_string_free(data, TRUE);
}

enum bt_plugin_status bt_plugin_find(const char *plugin_name,
		bt_bool fail_on_load_error, const struct bt_plugin **plugin_out)
{
	const char *system_plugin_dir;
	char *home_plugin_dir = NULL;
	const char *envvar;
	const struct bt_plugin *plugin = NULL;
	const struct bt_plugin_set *plugin_set = NULL;
	GPtrArray *dirs = NULL;
	int ret;
	enum bt_plugin_status status = BT_PLUGIN_STATUS_OK;
	size_t i, j;

	BT_ASSERT_PRE_NON_NULL(plugin_name, "Name");
	BT_ASSERT_PRE_NON_NULL(plugin_out, "Plugin (output)");
	BT_LOGI("Finding named plugin in standard directories and built-in plugins: "
		"name=\"%s\"", plugin_name);
	dirs = g_ptr_array_new_with_free_func((GDestroyNotify) destroy_gstring);
	if (!dirs) {
		BT_LOGE_STR("Failed to allocate a GPtrArray.");
		status = BT_PLUGIN_STATUS_NOMEM;
		goto end;
	}

	/*
	 * Search order is:
	 *
	 * 1. BABELTRACE_PLUGIN_PATH environment variable
	 *    (colon-separated list of directories)
	 * 2. ~/.local/lib/babeltrace2/plugins
	 * 3. Default system directory for Babeltrace plugins, usually
	 *    /usr/lib/babeltrace2/plugins or
	 *    /usr/local/lib/babeltrace2/plugins if installed
	 *    locally
	 * 4. Built-in plugins (static)
	 *
	 * Directories are searched non-recursively.
	 */
	envvar = getenv("BABELTRACE_PLUGIN_PATH");
	if (envvar) {
		ret = bt_common_append_plugin_path_dirs(envvar, dirs);
		if (ret) {
			BT_LOGE_STR("Failed to append plugin path to array of directories.");
			status = BT_PLUGIN_STATUS_NOMEM;
			goto end;
		}
	}

	home_plugin_dir = bt_common_get_home_plugin_path(BT_LOG_OUTPUT_LEVEL);
	if (home_plugin_dir) {
		GString *home_plugin_dir_str = g_string_new(home_plugin_dir);

		if (!home_plugin_dir_str) {
			BT_LOGE_STR("Failed to allocate a GString.");
			status = BT_PLUGIN_STATUS_NOMEM;
			goto end;
		}

		g_ptr_array_add(dirs, home_plugin_dir_str);
	}

	system_plugin_dir = bt_common_get_system_plugin_path();
	if (system_plugin_dir) {
		GString *system_plugin_dir_str =
			g_string_new(system_plugin_dir);

		if (!system_plugin_dir_str) {
			BT_LOGE_STR("Failed to allocate a GString.");
			status = BT_PLUGIN_STATUS_NOMEM;
			goto end;
		}

		g_ptr_array_add(dirs, system_plugin_dir_str);
	}

	for (i = 0; i < dirs->len; i++) {
		GString *dir = g_ptr_array_index(dirs, i);

		BT_OBJECT_PUT_REF_AND_RESET(plugin_set);

		/*
		 * Skip this if the directory does not exist because
		 * bt_plugin_find_all_from_dir() would log a warning.
		 */
		if (!g_file_test(dir->str, G_FILE_TEST_IS_DIR)) {
			BT_LOGI("Skipping nonexistent directory path: "
				"path=\"%s\"", dir->str);
			continue;
		}

		/* bt_plugin_find_all_from_dir() logs details/errors */
		status = bt_plugin_find_all_from_dir(dir->str, BT_FALSE,
			fail_on_load_error, &plugin_set);
		if (status < 0) {
			BT_ASSERT(!plugin_set);
			goto end;
		} else if (status == BT_PLUGIN_STATUS_NOT_FOUND) {
			BT_ASSERT(!plugin_set);
			BT_LOGI("No plugins found in directory: path=\"%s\"",
				dir->str);
			continue;
		}

		BT_ASSERT(status == BT_PLUGIN_STATUS_OK);
		BT_ASSERT(plugin_set);

		for (j = 0; j < plugin_set->plugins->len; j++) {
			const struct bt_plugin *candidate_plugin =
				g_ptr_array_index(plugin_set->plugins, j);

			if (strcmp(bt_plugin_get_name(candidate_plugin),
					plugin_name) == 0) {
				BT_LOGI("Plugin found in directory: name=\"%s\", path=\"%s\"",
					plugin_name, dir->str);
				*plugin_out = candidate_plugin;
				bt_object_get_no_null_check(*plugin_out);
				goto end;
			}
		}

		BT_LOGI("Plugin not found in directory: name=\"%s\", path=\"%s\"",
			plugin_name, dir->str);
	}

	BT_OBJECT_PUT_REF_AND_RESET(plugin_set);
	status = bt_plugin_find_all_from_static(fail_on_load_error,
		&plugin_set);
	if (status < 0) {
		BT_ASSERT(!plugin_set);
		goto end;
	}

	if (status == BT_PLUGIN_STATUS_NOT_FOUND) {
		BT_ASSERT(!plugin_set);
		BT_LOGI_STR("No plugins found in built-in plugins.");
		goto end;
	}

	BT_ASSERT(status == BT_PLUGIN_STATUS_OK);
	BT_ASSERT(plugin_set);

	for (j = 0; j < plugin_set->plugins->len; j++) {
		const struct bt_plugin *candidate_plugin =
			g_ptr_array_index(plugin_set->plugins, j);

		if (strcmp(bt_plugin_get_name(candidate_plugin),
				plugin_name) == 0) {
			BT_LOGI("Plugin found in built-in plugins: "
				"name=\"%s\"", plugin_name);
			*plugin_out = candidate_plugin;
			bt_object_get_no_null_check(*plugin_out);
			goto end;
		}
	}

	status = BT_PLUGIN_STATUS_NOT_FOUND;

end:
	free(home_plugin_dir);
	bt_object_put_ref(plugin_set);

	if (dirs) {
		g_ptr_array_free(dirs, TRUE);
	}

	if (status == BT_PLUGIN_STATUS_OK) {
		BT_LIB_LOGI("Found plugin in standard directories and built-in plugins: "
			"%!+l", plugin);
	} else if (status == BT_PLUGIN_STATUS_NOT_FOUND) {
		BT_LOGI("No plugin found in standard directories and built-in plugins: "
			"name=\"%s\"", plugin_name);
	}

	return status;
}

static struct {
	pthread_mutex_t lock;
	struct bt_plugin_set *plugin_set;
	bool recurse;
	bool fail_on_load_error;
	enum bt_plugin_status status;
} append_all_from_dir_info = {
	.lock = PTHREAD_MUTEX_INITIALIZER
};

static
int nftw_append_all_from_dir(const char *file,
		const struct stat *sb, int flag, struct FTW *s)
{
	int ret = 0;
	const char *name = file + s->base;

	/* Check for recursion */
	if (!append_all_from_dir_info.recurse && s->level > 1) {
		goto end;
	}

	switch (flag) {
	case FTW_F:
	{
		const struct bt_plugin_set *plugins_from_file = NULL;

		if (name[0] == '.') {
			/* Skip hidden files */
			BT_LOGI("Skipping hidden file: path=\"%s\"", file);
			goto end;
		}

		append_all_from_dir_info.status =
			bt_plugin_find_all_from_file(file,
				append_all_from_dir_info.fail_on_load_error,
				&plugins_from_file);
		if (append_all_from_dir_info.status == BT_PLUGIN_STATUS_OK) {
			size_t j;

			BT_ASSERT(plugins_from_file);

			for (j = 0; j < plugins_from_file->plugins->len; j++) {
				struct bt_plugin *plugin =
					g_ptr_array_index(plugins_from_file->plugins, j);

				BT_LIB_LOGI("Adding plugin to plugin set: "
					"plugin-path=\"%s\", %![plugin-]+l",
					file, plugin);
				bt_plugin_set_add_plugin(
					append_all_from_dir_info.plugin_set,
					plugin);
			}

			bt_object_put_ref(plugins_from_file);
			goto end;
		} else if (append_all_from_dir_info.status < 0) {
			/* bt_plugin_find_all_from_file() logs errors */
			BT_ASSERT(!plugins_from_file);
			ret = -1;
			goto end;
		}

		/*
		 * Not found in this file: this is no an error; continue
		 * walking the directories.
		 */
		BT_ASSERT(!plugins_from_file);
		BT_ASSERT(append_all_from_dir_info.status ==
			BT_PLUGIN_STATUS_NOT_FOUND);
		break;
	}
	case FTW_DNR:
		/* Continue to next file / directory. */
		BT_LOGI("Cannot enter directory: continuing: path=\"%s\"", file);
		break;
	case FTW_NS:
		/* Continue to next file / directory. */
		BT_LOGI("Cannot get file information: continuing: path=\"%s\"", file);
		break;
	}

end:
	return ret;
}

static
enum bt_plugin_status bt_plugin_create_append_all_from_dir(
		struct bt_plugin_set *plugin_set, const char *path,
		bt_bool recurse, bt_bool fail_on_load_error)
{
	int nftw_flags = FTW_PHYS;
	int ret;
	enum bt_plugin_status status;
	struct stat sb;

	BT_ASSERT(plugin_set);
	BT_ASSERT(path);
	BT_ASSERT(strlen(path) < PATH_MAX);

	/*
	 * Make sure that path exists and is accessible.
	 * This is necessary since Cygwin implementation of nftw() is not POSIX
	 * compliant. Cygwin nftw() implementation does not fail on non-existent
	 * path with ENOENT. Instead, it flags the directory as FTW_NS. FTW_NS during
	 * nftw_append_all_from_dir is not treated as an error since we are
	 * traversing the tree for plugin discovery.
	 */
	if (stat(path, &sb)) {
		BT_LOGW_ERRNO("Cannot open directory",
			": path=\"%s\", recurse=%d",
			path, recurse);
		status = BT_PLUGIN_STATUS_ERROR;
		goto end;
	}

	pthread_mutex_lock(&append_all_from_dir_info.lock);
	append_all_from_dir_info.plugin_set = plugin_set;
	append_all_from_dir_info.recurse = recurse;
	append_all_from_dir_info.status = BT_PLUGIN_STATUS_OK;
	append_all_from_dir_info.fail_on_load_error = fail_on_load_error;
	ret = nftw(path, nftw_append_all_from_dir,
		APPEND_ALL_FROM_DIR_NFDOPEN_MAX, nftw_flags);
	append_all_from_dir_info.plugin_set = NULL;
	status = append_all_from_dir_info.status;
	pthread_mutex_unlock(&append_all_from_dir_info.lock);
	if (ret) {
		BT_LOGW_ERRNO("Failed to walk directory",
			": path=\"%s\", recurse=%d",
			path, recurse);
		status = BT_PLUGIN_STATUS_ERROR;
		goto end;
	}

	if (status == BT_PLUGIN_STATUS_NOT_FOUND) {
		/*
		 * We're just appending in this function; even if
		 * nothing was found, it's still okay from the caller's
		 * perspective.
		 */
		status = BT_PLUGIN_STATUS_OK;
	}

end:
	return status;
}

enum bt_plugin_status bt_plugin_find_all_from_dir(const char *path,
		bt_bool recurse, bt_bool fail_on_load_error,
		const struct bt_plugin_set **plugin_set_out)
{
	enum bt_plugin_status status = BT_PLUGIN_STATUS_OK;

	BT_ASSERT_PRE_NON_NULL(plugin_set_out, "Plugin set (output)");
	BT_LOGI("Creating all plugins in directory: path=\"%s\", recurse=%d",
		path, recurse);
	*plugin_set_out = bt_plugin_set_create();
	if (!*plugin_set_out) {
		BT_LOGE_STR("Cannot create empty plugin set.");
		status = BT_PLUGIN_STATUS_NOMEM;
		goto error;
	}

	/*
	 * Append found plugins to array (never returns
	 * `BT_PLUGIN_STATUS_NOT_FOUND`)
	 */
	status = bt_plugin_create_append_all_from_dir((void *) *plugin_set_out,
		path, recurse, fail_on_load_error);
	if (status < 0) {
		/*
		 * bt_plugin_create_append_all_from_dir() handles
		 * `fail_on_load_error`, so this is a "real" error.
		 */
		BT_LOGW("Cannot append plugins found in directory: "
			"path=\"%s\", status=%s",
			path, bt_plugin_status_string(status));
		goto error;
	}

	BT_ASSERT(status == BT_PLUGIN_STATUS_OK);

	if ((*plugin_set_out)->plugins->len == 0) {
		/* Nothing was appended: not found */
		BT_LOGI("No plugins found in directory: path=\"%s\"", path);
		status = BT_PLUGIN_STATUS_NOT_FOUND;
		goto error;
	}

	BT_LOGI("Created %u plugins from directory: count=%u, path=\"%s\"",
		(*plugin_set_out)->plugins->len,
		(*plugin_set_out)->plugins->len, path);
	goto end;

error:
	BT_ASSERT(status != BT_PLUGIN_STATUS_OK);
	BT_OBJECT_PUT_REF_AND_RESET(*plugin_set_out);

end:
	return status;
}

const char *bt_plugin_get_name(const struct bt_plugin *plugin)
{
	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	return plugin->info.name_set ? plugin->info.name->str : NULL;
}

const char *bt_plugin_get_author(const struct bt_plugin *plugin)
{
	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	return plugin->info.author_set ? plugin->info.author->str : NULL;
}

const char *bt_plugin_get_license(const struct bt_plugin *plugin)
{
	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	return plugin->info.license_set ? plugin->info.license->str : NULL;
}

const char *bt_plugin_get_path(const struct bt_plugin *plugin)
{
	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	return plugin->info.path_set ? plugin->info.path->str : NULL;
}

const char *bt_plugin_get_description(const struct bt_plugin *plugin)
{
	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	return plugin->info.description_set ?
		plugin->info.description->str : NULL;
}

enum bt_property_availability bt_plugin_get_version(const struct bt_plugin *plugin,
		unsigned int *major, unsigned int *minor, unsigned int *patch,
		const char **extra)
{
	enum bt_property_availability avail =
		BT_PROPERTY_AVAILABILITY_AVAILABLE;

	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");

	if (!plugin->info.version_set) {
		BT_LIB_LOGD("Plugin's version is not set: %!+l", plugin);
		avail = BT_PROPERTY_AVAILABILITY_NOT_AVAILABLE;
		goto end;
	}

	if (major) {
		*major = plugin->info.version.major;
	}

	if (minor) {
		*minor = plugin->info.version.minor;
	}

	if (patch) {
		*patch = plugin->info.version.patch;
	}

	if (extra) {
		*extra = plugin->info.version.extra->str;
	}

end:
	return avail;
}

uint64_t bt_plugin_get_source_component_class_count(const struct bt_plugin *plugin)
{
	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	return (uint64_t) plugin->src_comp_classes->len;
}

uint64_t bt_plugin_get_filter_component_class_count(const struct bt_plugin *plugin)
{
	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	return (uint64_t) plugin->flt_comp_classes->len;
}

uint64_t bt_plugin_get_sink_component_class_count(const struct bt_plugin *plugin)
{
	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	return (uint64_t) plugin->sink_comp_classes->len;
}

static inline
struct bt_component_class *borrow_component_class_by_index(
		const struct bt_plugin *plugin, GPtrArray *comp_classes,
		uint64_t index)
{
	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	BT_ASSERT_PRE_VALID_INDEX(index, comp_classes->len);
	return g_ptr_array_index(comp_classes, index);
}

const struct bt_component_class_source *
bt_plugin_borrow_source_component_class_by_index_const(
		const struct bt_plugin *plugin, uint64_t index)
{
	return (const void *) borrow_component_class_by_index(plugin,
		plugin->src_comp_classes, index);
}

const struct bt_component_class_filter *
bt_plugin_borrow_filter_component_class_by_index_const(
		const struct bt_plugin *plugin, uint64_t index)
{
	return (const void *) borrow_component_class_by_index(plugin,
		plugin->flt_comp_classes, index);
}

const struct bt_component_class_sink *
bt_plugin_borrow_sink_component_class_by_index_const(
		const struct bt_plugin *plugin, uint64_t index)
{
	return (const void *) borrow_component_class_by_index(plugin,
		plugin->sink_comp_classes, index);
}

static inline
struct bt_component_class *borrow_component_class_by_name(
		const struct bt_plugin *plugin, GPtrArray *comp_classes,
		const char *name)
{
	struct bt_component_class *comp_class = NULL;
	size_t i;

	BT_ASSERT_PRE_NON_NULL(plugin, "Plugin");
	BT_ASSERT_PRE_NON_NULL(name, "Name");

	for (i = 0; i < comp_classes->len; i++) {
		struct bt_component_class *comp_class_candidate =
			g_ptr_array_index(comp_classes, i);
		const char *comp_class_cand_name =
			bt_component_class_get_name(comp_class_candidate);

		BT_ASSERT(comp_class_cand_name);

		if (strcmp(name, comp_class_cand_name) == 0) {
			comp_class = comp_class_candidate;
			break;
		}
	}

	return comp_class;
}

const struct bt_component_class_source *
bt_plugin_borrow_source_component_class_by_name_const(
		const struct bt_plugin *plugin, const char *name)
{
	return (const void *) borrow_component_class_by_name(plugin,
		plugin->src_comp_classes, name);
}

const struct bt_component_class_filter *
bt_plugin_borrow_filter_component_class_by_name_const(
		const struct bt_plugin *plugin, const char *name)
{
	return (const void *) borrow_component_class_by_name(plugin,
		plugin->flt_comp_classes, name);
}

const struct bt_component_class_sink *
bt_plugin_borrow_sink_component_class_by_name_const(
		const struct bt_plugin *plugin, const char *name)
{
	return (const void *) borrow_component_class_by_name(plugin,
		plugin->sink_comp_classes, name);
}

void bt_plugin_get_ref(const struct bt_plugin *plugin)
{
	bt_object_get_ref(plugin);
}

void bt_plugin_put_ref(const struct bt_plugin *plugin)
{
	bt_object_put_ref(plugin);
}

void bt_plugin_set_get_ref(const struct bt_plugin_set *plugin_set)
{
	bt_object_get_ref(plugin_set);
}

void bt_plugin_set_put_ref(const struct bt_plugin_set *plugin_set)
{
	bt_object_put_ref(plugin_set);
}
