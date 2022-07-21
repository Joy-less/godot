/*************************************************************************/
/*  editor_export.cpp                                                    */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2022 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2022 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "editor_export.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/extension/native_extension.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/file_access_encrypted.h"
#include "core/io/file_access_pack.h" // PACK_HEADER_MAGIC, PACK_FORMAT_VERSION
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/io/zip_io.h"
#include "core/object/script_language.h"
#include "core/version.h"
#include "editor/editor_file_system.h"
#include "editor/editor_node.h"
#include "editor/editor_paths.h"
#include "editor/editor_scale.h"
#include "editor/editor_settings.h"
#include "editor/plugins/script_editor_plugin.h"
#include "scene/resources/resource_format_text.h"

static int _get_pad(int p_alignment, int p_n) {
	int rest = p_n % p_alignment;
	int pad = 0;
	if (rest > 0) {
		pad = p_alignment - rest;
	};

	return pad;
}

#define PCK_PADDING 16

bool EditorExportPreset::_set(const StringName &p_name, const Variant &p_value) {
	if (values.has(p_name)) {
		values[p_name] = p_value;
		EditorExport::singleton->save_presets();
		return true;
	}

	return false;
}

bool EditorExportPreset::_get(const StringName &p_name, Variant &r_ret) const {
	if (values.has(p_name)) {
		r_ret = values[p_name];
		return true;
	}

	return false;
}

void EditorExportPreset::_get_property_list(List<PropertyInfo> *p_list) const {
	for (const PropertyInfo &E : properties) {
		if (platform->get_export_option_visibility(E.name, values)) {
			p_list->push_back(E);
		}
	}
}

Ref<EditorExportPlatform> EditorExportPreset::get_platform() const {
	return platform;
}

void EditorExportPreset::update_files_to_export() {
	Vector<String> to_remove;
	for (const String &E : selected_files) {
		if (!FileAccess::exists(E)) {
			to_remove.push_back(E);
		}
	}
	for (int i = 0; i < to_remove.size(); ++i) {
		selected_files.erase(to_remove[i]);
	}
}

Vector<String> EditorExportPreset::get_files_to_export() const {
	Vector<String> files;
	for (const String &E : selected_files) {
		files.push_back(E);
	}
	return files;
}

void EditorExportPreset::set_name(const String &p_name) {
	name = p_name;
	EditorExport::singleton->save_presets();
}

String EditorExportPreset::get_name() const {
	return name;
}

void EditorExportPreset::set_runnable(bool p_enable) {
	runnable = p_enable;
	EditorExport::singleton->save_presets();
}

bool EditorExportPreset::is_runnable() const {
	return runnable;
}

void EditorExportPreset::set_export_filter(ExportFilter p_filter) {
	export_filter = p_filter;
	EditorExport::singleton->save_presets();
}

EditorExportPreset::ExportFilter EditorExportPreset::get_export_filter() const {
	return export_filter;
}

void EditorExportPreset::set_include_filter(const String &p_include) {
	include_filter = p_include;
	EditorExport::singleton->save_presets();
}

String EditorExportPreset::get_include_filter() const {
	return include_filter;
}

void EditorExportPreset::set_export_path(const String &p_path) {
	export_path = p_path;
	/* NOTE(SonerSound): if there is a need to implement a PropertyHint that specifically indicates a relative path,
	 * this should be removed. */
	if (export_path.is_absolute_path()) {
		String res_path = OS::get_singleton()->get_resource_dir();
		export_path = res_path.path_to_file(export_path);
	}
	EditorExport::singleton->save_presets();
}

String EditorExportPreset::get_export_path() const {
	return export_path;
}

void EditorExportPreset::set_exclude_filter(const String &p_exclude) {
	exclude_filter = p_exclude;
	EditorExport::singleton->save_presets();
}

String EditorExportPreset::get_exclude_filter() const {
	return exclude_filter;
}

void EditorExportPreset::add_export_file(const String &p_path) {
	selected_files.insert(p_path);
	EditorExport::singleton->save_presets();
}

void EditorExportPreset::remove_export_file(const String &p_path) {
	selected_files.erase(p_path);
	EditorExport::singleton->save_presets();
}

bool EditorExportPreset::has_export_file(const String &p_path) {
	return selected_files.has(p_path);
}

void EditorExportPreset::set_custom_features(const String &p_custom_features) {
	custom_features = p_custom_features;
	EditorExport::singleton->save_presets();
}

String EditorExportPreset::get_custom_features() const {
	return custom_features;
}

void EditorExportPreset::set_enc_in_filter(const String &p_filter) {
	enc_in_filters = p_filter;
	EditorExport::singleton->save_presets();
}

String EditorExportPreset::get_enc_in_filter() const {
	return enc_in_filters;
}

void EditorExportPreset::set_enc_ex_filter(const String &p_filter) {
	enc_ex_filters = p_filter;
	EditorExport::singleton->save_presets();
}

String EditorExportPreset::get_enc_ex_filter() const {
	return enc_ex_filters;
}

void EditorExportPreset::set_enc_pck(bool p_enabled) {
	enc_pck = p_enabled;
	EditorExport::singleton->save_presets();
}

bool EditorExportPreset::get_enc_pck() const {
	return enc_pck;
}

void EditorExportPreset::set_enc_directory(bool p_enabled) {
	enc_directory = p_enabled;
	EditorExport::singleton->save_presets();
}

bool EditorExportPreset::get_enc_directory() const {
	return enc_directory;
}

void EditorExportPreset::set_script_export_mode(int p_mode) {
	script_mode = p_mode;
	EditorExport::singleton->save_presets();
}

int EditorExportPreset::get_script_export_mode() const {
	return script_mode;
}

void EditorExportPreset::set_script_encryption_key(const String &p_key) {
	script_key = p_key;
	EditorExport::singleton->save_presets();
}

String EditorExportPreset::get_script_encryption_key() const {
	return script_key;
}

///////////////////////////////////

bool EditorExportPlatform::fill_log_messages(RichTextLabel *p_log, Error p_err) {
	bool has_messages = false;

	int msg_count = get_message_count();

	p_log->add_text(TTR("Project export for platform:") + " ");
	p_log->add_image(get_logo(), 16 * EDSCALE, 16 * EDSCALE, Color(1.0, 1.0, 1.0), INLINE_ALIGNMENT_CENTER);
	p_log->add_text(" ");
	p_log->add_text(get_name());
	p_log->add_text(" - ");
	if (p_err == OK) {
		if (get_worst_message_type() >= EditorExportPlatform::EXPORT_MESSAGE_WARNING) {
			p_log->add_image(EditorNode::get_singleton()->get_gui_base()->get_theme_icon(SNAME("StatusWarning"), SNAME("EditorIcons")), 16 * EDSCALE, 16 * EDSCALE, Color(1.0, 1.0, 1.0), INLINE_ALIGNMENT_CENTER);
			p_log->add_text(" ");
			p_log->add_text(TTR("Completed with errors."));
			has_messages = true;
		} else {
			p_log->add_image(EditorNode::get_singleton()->get_gui_base()->get_theme_icon(SNAME("StatusSuccess"), SNAME("EditorIcons")), 16 * EDSCALE, 16 * EDSCALE, Color(1.0, 1.0, 1.0), INLINE_ALIGNMENT_CENTER);
			p_log->add_text(" ");
			p_log->add_text(TTR("Completed sucessfully."));
			if (msg_count > 0) {
				has_messages = true;
			}
		}
	} else {
		p_log->add_image(EditorNode::get_singleton()->get_gui_base()->get_theme_icon(SNAME("StatusError"), SNAME("EditorIcons")), 16 * EDSCALE, 16 * EDSCALE, Color(1.0, 1.0, 1.0), INLINE_ALIGNMENT_CENTER);
		p_log->add_text(" ");
		p_log->add_text(TTR("Failed."));
		has_messages = true;
	}
	p_log->add_newline();

	if (msg_count) {
		p_log->push_table(2);
		p_log->set_table_column_expand(0, false);
		p_log->set_table_column_expand(1, true);
		for (int m = 0; m < msg_count; m++) {
			EditorExportPlatform::ExportMessage msg = get_message(m);
			Color color = EditorNode::get_singleton()->get_gui_base()->get_theme_color(SNAME("font_color"), SNAME("Label"));
			Ref<Texture> icon;

			switch (msg.msg_type) {
				case EditorExportPlatform::EXPORT_MESSAGE_INFO: {
					color = EditorNode::get_singleton()->get_gui_base()->get_theme_color(SNAME("font_color"), SNAME("Editor")) * Color(1, 1, 1, 0.6);
				} break;
				case EditorExportPlatform::EXPORT_MESSAGE_WARNING: {
					icon = EditorNode::get_singleton()->get_gui_base()->get_theme_icon(SNAME("Warning"), SNAME("EditorIcons"));
					color = EditorNode::get_singleton()->get_gui_base()->get_theme_color(SNAME("warning_color"), SNAME("Editor"));
				} break;
				case EditorExportPlatform::EXPORT_MESSAGE_ERROR: {
					icon = EditorNode::get_singleton()->get_gui_base()->get_theme_icon(SNAME("Error"), SNAME("EditorIcons"));
					color = EditorNode::get_singleton()->get_gui_base()->get_theme_color(SNAME("error_color"), SNAME("Editor"));
				} break;
				default:
					break;
			}

			p_log->push_cell();
			p_log->add_text("\t");
			if (icon.is_valid()) {
				p_log->add_image(icon);
			}
			p_log->pop();

			p_log->push_cell();
			p_log->push_color(color);
			p_log->add_text(vformat("[%s]: %s", msg.category, msg.text));
			p_log->pop();
			p_log->pop();
		}
		p_log->pop();
		p_log->add_newline();
	}
	p_log->add_newline();
	return has_messages;
}

void EditorExportPlatform::gen_debug_flags(Vector<String> &r_flags, int p_flags) {
	String host = EditorSettings::get_singleton()->get("network/debug/remote_host");
	int remote_port = (int)EditorSettings::get_singleton()->get("network/debug/remote_port");

	if (p_flags & DEBUG_FLAG_REMOTE_DEBUG_LOCALHOST) {
		host = "localhost";
	}

	if (p_flags & DEBUG_FLAG_DUMB_CLIENT) {
		int port = EditorSettings::get_singleton()->get("filesystem/file_server/port");
		String passwd = EditorSettings::get_singleton()->get("filesystem/file_server/password");
		r_flags.push_back("--remote-fs");
		r_flags.push_back(host + ":" + itos(port));
		if (!passwd.is_empty()) {
			r_flags.push_back("--remote-fs-password");
			r_flags.push_back(passwd);
		}
	}

	if (p_flags & DEBUG_FLAG_REMOTE_DEBUG) {
		r_flags.push_back("--remote-debug");

		r_flags.push_back(get_debug_protocol() + host + ":" + String::num(remote_port));

		List<String> breakpoints;
		ScriptEditor::get_singleton()->get_breakpoints(&breakpoints);

		if (breakpoints.size()) {
			r_flags.push_back("--breakpoints");
			String bpoints;
			for (const List<String>::Element *E = breakpoints.front(); E; E = E->next()) {
				bpoints += E->get().replace(" ", "%20");
				if (E->next()) {
					bpoints += ",";
				}
			}

			r_flags.push_back(bpoints);
		}
	}

	if (p_flags & DEBUG_FLAG_VIEW_COLLISONS) {
		r_flags.push_back("--debug-collisions");
	}

	if (p_flags & DEBUG_FLAG_VIEW_NAVIGATION) {
		r_flags.push_back("--debug-navigation");
	}
}

Error EditorExportPlatform::_save_pack_file(void *p_userdata, const String &p_path, const Vector<uint8_t> &p_data, int p_file, int p_total, const Vector<String> &p_enc_in_filters, const Vector<String> &p_enc_ex_filters, const Vector<uint8_t> &p_key) {
	ERR_FAIL_COND_V_MSG(p_total < 1, ERR_PARAMETER_RANGE_ERROR, "Must select at least one file to export.");

	PackData *pd = (PackData *)p_userdata;

	SavedData sd;
	sd.path_utf8 = p_path.utf8();
	sd.ofs = pd->f->get_position();
	sd.size = p_data.size();
	sd.encrypted = false;

	for (int i = 0; i < p_enc_in_filters.size(); ++i) {
		if (p_path.matchn(p_enc_in_filters[i]) || p_path.replace("res://", "").matchn(p_enc_in_filters[i])) {
			sd.encrypted = true;
			break;
		}
	}

	for (int i = 0; i < p_enc_ex_filters.size(); ++i) {
		if (p_path.matchn(p_enc_ex_filters[i]) || p_path.replace("res://", "").matchn(p_enc_ex_filters[i])) {
			sd.encrypted = false;
			break;
		}
	}

	Ref<FileAccessEncrypted> fae;
	Ref<FileAccess> ftmp = pd->f;

	if (sd.encrypted) {
		fae.instantiate();
		ERR_FAIL_COND_V(fae.is_null(), ERR_SKIP);

		Error err = fae->open_and_parse(ftmp, p_key, FileAccessEncrypted::MODE_WRITE_AES256, false);
		ERR_FAIL_COND_V(err != OK, ERR_SKIP);
		ftmp = fae;
	}

	// Store file content.
	ftmp->store_buffer(p_data.ptr(), p_data.size());

	if (fae.is_valid()) {
		ftmp.unref();
		fae.unref();
	}

	int pad = _get_pad(PCK_PADDING, pd->f->get_position());
	for (int i = 0; i < pad; i++) {
		pd->f->store_8(Math::rand() % 256);
	}

	// Store MD5 of original file.
	{
		unsigned char hash[16];
		CryptoCore::md5(p_data.ptr(), p_data.size(), hash);
		sd.md5.resize(16);
		for (int i = 0; i < 16; i++) {
			sd.md5.write[i] = hash[i];
		}
	}

	pd->file_ofs.push_back(sd);

	if (pd->ep->step(TTR("Storing File:") + " " + p_path, 2 + p_file * 100 / p_total, false)) {
		return ERR_SKIP;
	}

	return OK;
}

Error EditorExportPlatform::_save_zip_file(void *p_userdata, const String &p_path, const Vector<uint8_t> &p_data, int p_file, int p_total, const Vector<String> &p_enc_in_filters, const Vector<String> &p_enc_ex_filters, const Vector<uint8_t> &p_key) {
	ERR_FAIL_COND_V_MSG(p_total < 1, ERR_PARAMETER_RANGE_ERROR, "Must select at least one file to export.");

	String path = p_path.replace_first("res://", "");

	ZipData *zd = (ZipData *)p_userdata;

	zipFile zip = (zipFile)zd->zip;

	zipOpenNewFileInZip(zip,
			path.utf8().get_data(),
			nullptr,
			nullptr,
			0,
			nullptr,
			0,
			nullptr,
			Z_DEFLATED,
			Z_DEFAULT_COMPRESSION);

	zipWriteInFileInZip(zip, p_data.ptr(), p_data.size());
	zipCloseFileInZip(zip);

	if (zd->ep->step(TTR("Storing File:") + " " + p_path, 2 + p_file * 100 / p_total, false)) {
		return ERR_SKIP;
	}

	return OK;
}

Ref<ImageTexture> EditorExportPlatform::get_option_icon(int p_index) const {
	Ref<Theme> theme = EditorNode::get_singleton()->get_editor_theme();
	ERR_FAIL_COND_V(theme.is_null(), Ref<ImageTexture>());
	if (EditorNode::get_singleton()->get_main_control()->is_layout_rtl()) {
		return theme->get_icon(SNAME("PlayBackwards"), SNAME("EditorIcons"));
	} else {
		return theme->get_icon(SNAME("Play"), SNAME("EditorIcons"));
	}
}

String EditorExportPlatform::find_export_template(String template_file_name, String *err) const {
	String current_version = VERSION_FULL_CONFIG;
	String template_path = EditorSettings::get_singleton()->get_templates_dir().plus_file(current_version).plus_file(template_file_name);

	if (FileAccess::exists(template_path)) {
		return template_path;
	}

	// Not found
	if (err) {
		*err += TTR("No export template found at the expected path:") + "\n" + template_path + "\n";
	}
	return String();
}

bool EditorExportPlatform::exists_export_template(String template_file_name, String *err) const {
	return find_export_template(template_file_name, err) != "";
}

Ref<EditorExportPreset> EditorExportPlatform::create_preset() {
	Ref<EditorExportPreset> preset;
	preset.instantiate();
	preset->platform = Ref<EditorExportPlatform>(this);

	List<ExportOption> options;
	get_export_options(&options);

	for (const ExportOption &E : options) {
		preset->properties.push_back(E.option);
		preset->values[E.option.name] = E.default_value;
	}

	return preset;
}

void EditorExportPlatform::_export_find_resources(EditorFileSystemDirectory *p_dir, HashSet<String> &p_paths) {
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		_export_find_resources(p_dir->get_subdir(i), p_paths);
	}

	for (int i = 0; i < p_dir->get_file_count(); i++) {
		if (p_dir->get_file_type(i) == "TextFile") {
			continue;
		}
		p_paths.insert(p_dir->get_file_path(i));
	}
}

void EditorExportPlatform::_export_find_dependencies(const String &p_path, HashSet<String> &p_paths) {
	if (p_paths.has(p_path)) {
		return;
	}

	p_paths.insert(p_path);

	EditorFileSystemDirectory *dir;
	int file_idx;
	dir = EditorFileSystem::get_singleton()->find_file(p_path, &file_idx);
	if (!dir) {
		return;
	}

	Vector<String> deps = dir->get_file_deps(file_idx);

	for (int i = 0; i < deps.size(); i++) {
		_export_find_dependencies(deps[i], p_paths);
	}
}

void EditorExportPlatform::_edit_files_with_filter(Ref<DirAccess> &da, const Vector<String> &p_filters, HashSet<String> &r_list, bool exclude) {
	da->list_dir_begin();
	String cur_dir = da->get_current_dir().replace("\\", "/");
	if (!cur_dir.ends_with("/")) {
		cur_dir += "/";
	}
	String cur_dir_no_prefix = cur_dir.replace("res://", "");

	Vector<String> dirs;
	String f = da->get_next();
	while (!f.is_empty()) {
		if (da->current_is_dir()) {
			dirs.push_back(f);
		} else {
			String fullpath = cur_dir + f;
			// Test also against path without res:// so that filters like `file.txt` can work.
			String fullpath_no_prefix = cur_dir_no_prefix + f;
			for (int i = 0; i < p_filters.size(); ++i) {
				if (fullpath.matchn(p_filters[i]) || fullpath_no_prefix.matchn(p_filters[i])) {
					if (!exclude) {
						r_list.insert(fullpath);
					} else {
						r_list.erase(fullpath);
					}
				}
			}
		}
		f = da->get_next();
	}

	da->list_dir_end();

	for (int i = 0; i < dirs.size(); ++i) {
		String dir = dirs[i];
		if (dir.begins_with(".")) {
			continue;
		}

		if (EditorFileSystem::_should_skip_directory(cur_dir + dir)) {
			continue;
		}

		da->change_dir(dir);
		_edit_files_with_filter(da, p_filters, r_list, exclude);
		da->change_dir("..");
	}
}

void EditorExportPlatform::_edit_filter_list(HashSet<String> &r_list, const String &p_filter, bool exclude) {
	if (p_filter.is_empty()) {
		return;
	}
	Vector<String> split = p_filter.split(",");
	Vector<String> filters;
	for (int i = 0; i < split.size(); i++) {
		String f = split[i].strip_edges();
		if (f.is_empty()) {
			continue;
		}
		filters.push_back(f);
	}

	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
	ERR_FAIL_COND(da.is_null());
	_edit_files_with_filter(da, filters, r_list, exclude);
}

void EditorExportPlugin::set_export_preset(const Ref<EditorExportPreset> &p_preset) {
	if (p_preset.is_valid()) {
		export_preset = p_preset;
	}
}

Ref<EditorExportPreset> EditorExportPlugin::get_export_preset() const {
	return export_preset;
}

void EditorExportPlugin::add_file(const String &p_path, const Vector<uint8_t> &p_file, bool p_remap) {
	ExtraFile ef;
	ef.data = p_file;
	ef.path = p_path;
	ef.remap = p_remap;
	extra_files.push_back(ef);
}

void EditorExportPlugin::add_shared_object(const String &p_path, const Vector<String> &p_tags, const String &p_target) {
	shared_objects.push_back(SharedObject(p_path, p_tags, p_target));
}

void EditorExportPlugin::add_ios_framework(const String &p_path) {
	ios_frameworks.push_back(p_path);
}

void EditorExportPlugin::add_ios_embedded_framework(const String &p_path) {
	ios_embedded_frameworks.push_back(p_path);
}

Vector<String> EditorExportPlugin::get_ios_frameworks() const {
	return ios_frameworks;
}

Vector<String> EditorExportPlugin::get_ios_embedded_frameworks() const {
	return ios_embedded_frameworks;
}

void EditorExportPlugin::add_ios_plist_content(const String &p_plist_content) {
	ios_plist_content += p_plist_content + "\n";
}

String EditorExportPlugin::get_ios_plist_content() const {
	return ios_plist_content;
}

void EditorExportPlugin::add_ios_linker_flags(const String &p_flags) {
	if (ios_linker_flags.length() > 0) {
		ios_linker_flags += ' ';
	}
	ios_linker_flags += p_flags;
}

String EditorExportPlugin::get_ios_linker_flags() const {
	return ios_linker_flags;
}

void EditorExportPlugin::add_ios_bundle_file(const String &p_path) {
	ios_bundle_files.push_back(p_path);
}

Vector<String> EditorExportPlugin::get_ios_bundle_files() const {
	return ios_bundle_files;
}

void EditorExportPlugin::add_ios_cpp_code(const String &p_code) {
	ios_cpp_code += p_code;
}

String EditorExportPlugin::get_ios_cpp_code() const {
	return ios_cpp_code;
}

void EditorExportPlugin::add_macos_plugin_file(const String &p_path) {
	macos_plugin_files.push_back(p_path);
}

const Vector<String> &EditorExportPlugin::get_macos_plugin_files() const {
	return macos_plugin_files;
}

void EditorExportPlugin::add_ios_project_static_lib(const String &p_path) {
	ios_project_static_libs.push_back(p_path);
}

Vector<String> EditorExportPlugin::get_ios_project_static_libs() const {
	return ios_project_static_libs;
}

void EditorExportPlugin::_export_file_script(const String &p_path, const String &p_type, const Vector<String> &p_features) {
	GDVIRTUAL_CALL(_export_file, p_path, p_type, p_features);
}

void EditorExportPlugin::_export_begin_script(const Vector<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	GDVIRTUAL_CALL(_export_begin, p_features, p_debug, p_path, p_flags);
}

void EditorExportPlugin::_export_end_script() {
	GDVIRTUAL_CALL(_export_end);
}

void EditorExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
}

void EditorExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
}

void EditorExportPlugin::skip() {
	skipped = true;
}

void EditorExportPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_shared_object", "path", "tags", "target"), &EditorExportPlugin::add_shared_object);
	ClassDB::bind_method(D_METHOD("add_ios_project_static_lib", "path"), &EditorExportPlugin::add_ios_project_static_lib);
	ClassDB::bind_method(D_METHOD("add_file", "path", "file", "remap"), &EditorExportPlugin::add_file);
	ClassDB::bind_method(D_METHOD("add_ios_framework", "path"), &EditorExportPlugin::add_ios_framework);
	ClassDB::bind_method(D_METHOD("add_ios_embedded_framework", "path"), &EditorExportPlugin::add_ios_embedded_framework);
	ClassDB::bind_method(D_METHOD("add_ios_plist_content", "plist_content"), &EditorExportPlugin::add_ios_plist_content);
	ClassDB::bind_method(D_METHOD("add_ios_linker_flags", "flags"), &EditorExportPlugin::add_ios_linker_flags);
	ClassDB::bind_method(D_METHOD("add_ios_bundle_file", "path"), &EditorExportPlugin::add_ios_bundle_file);
	ClassDB::bind_method(D_METHOD("add_ios_cpp_code", "code"), &EditorExportPlugin::add_ios_cpp_code);
	ClassDB::bind_method(D_METHOD("add_macos_plugin_file", "path"), &EditorExportPlugin::add_macos_plugin_file);
	ClassDB::bind_method(D_METHOD("skip"), &EditorExportPlugin::skip);

	GDVIRTUAL_BIND(_export_file, "path", "type", "features");
	GDVIRTUAL_BIND(_export_begin, "features", "is_debug", "path", "flags");
	GDVIRTUAL_BIND(_export_end);
}

EditorExportPlugin::EditorExportPlugin() {
}

EditorExportPlatform::FeatureContainers EditorExportPlatform::get_feature_containers(const Ref<EditorExportPreset> &p_preset, bool p_debug) {
	Ref<EditorExportPlatform> platform = p_preset->get_platform();
	List<String> feature_list;
	platform->get_platform_features(&feature_list);
	platform->get_preset_features(p_preset, &feature_list);

	FeatureContainers result;
	for (const String &E : feature_list) {
		result.features.insert(E);
		result.features_pv.push_back(E);
	}

	if (p_debug) {
		result.features.insert("debug");
		result.features_pv.push_back("debug");
	} else {
		result.features.insert("release");
		result.features_pv.push_back("release");
	}

	if (!p_preset->get_custom_features().is_empty()) {
		Vector<String> tmp_custom_list = p_preset->get_custom_features().split(",");

		for (int i = 0; i < tmp_custom_list.size(); i++) {
			String f = tmp_custom_list[i].strip_edges();
			if (!f.is_empty()) {
				result.features.insert(f);
				result.features_pv.push_back(f);
			}
		}
	}

	return result;
}

EditorExportPlatform::ExportNotifier::ExportNotifier(EditorExportPlatform &p_platform, const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, int p_flags) {
	FeatureContainers features = p_platform.get_feature_containers(p_preset, p_debug);
	Vector<Ref<EditorExportPlugin>> export_plugins = EditorExport::get_singleton()->get_export_plugins();
	//initial export plugin callback
	for (int i = 0; i < export_plugins.size(); i++) {
		if (export_plugins[i]->get_script_instance()) { //script based
			export_plugins.write[i]->_export_begin_script(features.features_pv, p_debug, p_path, p_flags);
		} else {
			export_plugins.write[i]->_export_begin(features.features, p_debug, p_path, p_flags);
		}
	}
}

EditorExportPlatform::ExportNotifier::~ExportNotifier() {
	Vector<Ref<EditorExportPlugin>> export_plugins = EditorExport::get_singleton()->get_export_plugins();
	for (int i = 0; i < export_plugins.size(); i++) {
		if (export_plugins[i]->get_script_instance()) {
			export_plugins.write[i]->_export_end_script();
		}
		export_plugins.write[i]->_export_end();
	}
}

Error EditorExportPlatform::export_project_files(const Ref<EditorExportPreset> &p_preset, bool p_debug, EditorExportSaveFunction p_func, void *p_udata, EditorExportSaveSharedObject p_so_func) {
	//figure out paths of files that will be exported
	HashSet<String> paths;
	Vector<String> path_remaps;

	if (p_preset->get_export_filter() == EditorExportPreset::EXPORT_ALL_RESOURCES) {
		//find stuff
		_export_find_resources(EditorFileSystem::get_singleton()->get_filesystem(), paths);
	} else if (p_preset->get_export_filter() == EditorExportPreset::EXCLUDE_SELECTED_RESOURCES) {
		_export_find_resources(EditorFileSystem::get_singleton()->get_filesystem(), paths);
		Vector<String> files = p_preset->get_files_to_export();
		for (int i = 0; i < files.size(); i++) {
			paths.erase(files[i]);
		}
	} else {
		bool scenes_only = p_preset->get_export_filter() == EditorExportPreset::EXPORT_SELECTED_SCENES;

		Vector<String> files = p_preset->get_files_to_export();
		for (int i = 0; i < files.size(); i++) {
			if (scenes_only && ResourceLoader::get_resource_type(files[i]) != "PackedScene") {
				continue;
			}

			_export_find_dependencies(files[i], paths);
		}

		// Add autoload resources and their dependencies
		List<PropertyInfo> props;
		ProjectSettings::get_singleton()->get_property_list(&props);

		for (const PropertyInfo &pi : props) {
			if (!pi.name.begins_with("autoload/")) {
				continue;
			}

			String autoload_path = ProjectSettings::get_singleton()->get(pi.name);

			if (autoload_path.begins_with("*")) {
				autoload_path = autoload_path.substr(1);
			}

			_export_find_dependencies(autoload_path, paths);
		}
	}

	//add native icons to non-resource include list
	_edit_filter_list(paths, String("*.icns"), false);
	_edit_filter_list(paths, String("*.ico"), false);

	_edit_filter_list(paths, p_preset->get_include_filter(), false);
	_edit_filter_list(paths, p_preset->get_exclude_filter(), true);

	// Ignore import files, since these are automatically added to the jar later with the resources
	_edit_filter_list(paths, String("*.import"), true);

	// Get encryption filters.
	bool enc_pck = p_preset->get_enc_pck();
	Vector<String> enc_in_filters;
	Vector<String> enc_ex_filters;
	Vector<uint8_t> key;

	if (enc_pck) {
		Vector<String> enc_in_split = p_preset->get_enc_in_filter().split(",");
		for (int i = 0; i < enc_in_split.size(); i++) {
			String f = enc_in_split[i].strip_edges();
			if (f.is_empty()) {
				continue;
			}
			enc_in_filters.push_back(f);
		}

		Vector<String> enc_ex_split = p_preset->get_enc_ex_filter().split(",");
		for (int i = 0; i < enc_ex_split.size(); i++) {
			String f = enc_ex_split[i].strip_edges();
			if (f.is_empty()) {
				continue;
			}
			enc_ex_filters.push_back(f);
		}

		// Get encryption key.
		String script_key = p_preset->get_script_encryption_key().to_lower();
		key.resize(32);
		if (script_key.length() == 64) {
			for (int i = 0; i < 32; i++) {
				int v = 0;
				if (i * 2 < script_key.length()) {
					char32_t ct = script_key[i * 2];
					if (is_digit(ct)) {
						ct = ct - '0';
					} else if (ct >= 'a' && ct <= 'f') {
						ct = 10 + ct - 'a';
					}
					v |= ct << 4;
				}

				if (i * 2 + 1 < script_key.length()) {
					char32_t ct = script_key[i * 2 + 1];
					if (is_digit(ct)) {
						ct = ct - '0';
					} else if (ct >= 'a' && ct <= 'f') {
						ct = 10 + ct - 'a';
					}
					v |= ct;
				}
				key.write[i] = v;
			}
		}
	}

	Error err = OK;
	Vector<Ref<EditorExportPlugin>> export_plugins = EditorExport::get_singleton()->get_export_plugins();

	for (int i = 0; i < export_plugins.size(); i++) {
		export_plugins.write[i]->set_export_preset(p_preset);

		if (p_so_func) {
			for (int j = 0; j < export_plugins[i]->shared_objects.size(); j++) {
				err = p_so_func(p_udata, export_plugins[i]->shared_objects[j]);
				if (err != OK) {
					return err;
				}
			}
		}
		for (int j = 0; j < export_plugins[i]->extra_files.size(); j++) {
			err = p_func(p_udata, export_plugins[i]->extra_files[j].path, export_plugins[i]->extra_files[j].data, 0, paths.size(), enc_in_filters, enc_ex_filters, key);
			if (err != OK) {
				return err;
			}
		}

		export_plugins.write[i]->_clear();
	}

	FeatureContainers feature_containers = get_feature_containers(p_preset, p_debug);
	HashSet<String> &features = feature_containers.features;
	Vector<String> &features_pv = feature_containers.features_pv;

	//store everything in the export medium
	int idx = 0;
	int total = paths.size();

	for (const String &E : paths) {
		String path = E;
		String type = ResourceLoader::get_resource_type(path);

		if (FileAccess::exists(path + ".import")) {
			//file is imported, replace by what it imports
			Ref<ConfigFile> config;
			config.instantiate();
			err = config->load(path + ".import");
			if (err != OK) {
				ERR_PRINT("Could not parse: '" + path + "', not exported.");
				continue;
			}

			String importer_type = config->get_value("remap", "importer");

			if (importer_type == "keep") {
				//just keep file as-is
				Vector<uint8_t> array = FileAccess::get_file_as_array(path);
				err = p_func(p_udata, path, array, idx, total, enc_in_filters, enc_ex_filters, key);

				if (err != OK) {
					return err;
				}

				continue;
			}

			List<String> remaps;
			config->get_section_keys("remap", &remaps);

			HashSet<String> remap_features;

			for (const String &F : remaps) {
				String remap = F;
				String feature = remap.get_slice(".", 1);
				if (features.has(feature)) {
					remap_features.insert(feature);
				}
			}

			if (remap_features.size() > 1) {
				this->resolve_platform_feature_priorities(p_preset, remap_features);
			}

			err = OK;

			for (const String &F : remaps) {
				String remap = F;
				if (remap == "path") {
					String remapped_path = config->get_value("remap", remap);
					Vector<uint8_t> array = FileAccess::get_file_as_array(remapped_path);
					err = p_func(p_udata, remapped_path, array, idx, total, enc_in_filters, enc_ex_filters, key);
				} else if (remap.begins_with("path.")) {
					String feature = remap.get_slice(".", 1);

					if (remap_features.has(feature)) {
						String remapped_path = config->get_value("remap", remap);
						Vector<uint8_t> array = FileAccess::get_file_as_array(remapped_path);
						err = p_func(p_udata, remapped_path, array, idx, total, enc_in_filters, enc_ex_filters, key);
					}
				}
			}

			if (err != OK) {
				return err;
			}

			//also save the .import file
			Vector<uint8_t> array = FileAccess::get_file_as_array(path + ".import");
			err = p_func(p_udata, path + ".import", array, idx, total, enc_in_filters, enc_ex_filters, key);

			if (err != OK) {
				return err;
			}

		} else {
			bool do_export = true;
			for (int i = 0; i < export_plugins.size(); i++) {
				if (export_plugins[i]->get_script_instance()) { //script based
					export_plugins.write[i]->_export_file_script(path, type, features_pv);
				} else {
					export_plugins.write[i]->_export_file(path, type, features);
				}
				if (p_so_func) {
					for (int j = 0; j < export_plugins[i]->shared_objects.size(); j++) {
						err = p_so_func(p_udata, export_plugins[i]->shared_objects[j]);
						if (err != OK) {
							return err;
						}
					}
				}

				for (int j = 0; j < export_plugins[i]->extra_files.size(); j++) {
					err = p_func(p_udata, export_plugins[i]->extra_files[j].path, export_plugins[i]->extra_files[j].data, idx, total, enc_in_filters, enc_ex_filters, key);
					if (err != OK) {
						return err;
					}
					if (export_plugins[i]->extra_files[j].remap) {
						do_export = false; //if remap, do not
						path_remaps.push_back(path);
						path_remaps.push_back(export_plugins[i]->extra_files[j].path);
					}
				}

				if (export_plugins[i]->skipped) {
					do_export = false;
				}
				export_plugins.write[i]->_clear();

				if (!do_export) {
					break; //apologies, not exporting
				}
			}
			//just store it as it comes
			if (do_export) {
				Vector<uint8_t> array = FileAccess::get_file_as_array(path);
				err = p_func(p_udata, path, array, idx, total, enc_in_filters, enc_ex_filters, key);
				if (err != OK) {
					return err;
				}
			}
		}

		idx++;
	}

	//save config!

	Vector<String> custom_list;

	if (!p_preset->get_custom_features().is_empty()) {
		Vector<String> tmp_custom_list = p_preset->get_custom_features().split(",");

		for (int i = 0; i < tmp_custom_list.size(); i++) {
			String f = tmp_custom_list[i].strip_edges();
			if (!f.is_empty()) {
				custom_list.push_back(f);
			}
		}
	}

	ProjectSettings::CustomMap custom_map;
	if (path_remaps.size()) {
		if (true) { //new remap mode, use always as it's friendlier with multiple .pck exports
			for (int i = 0; i < path_remaps.size(); i += 2) {
				String from = path_remaps[i];
				String to = path_remaps[i + 1];
				String remap_file = "[remap]\n\npath=\"" + to.c_escape() + "\"\n";
				CharString utf8 = remap_file.utf8();
				Vector<uint8_t> new_file;
				new_file.resize(utf8.length());
				for (int j = 0; j < utf8.length(); j++) {
					new_file.write[j] = utf8[j];
				}

				err = p_func(p_udata, from + ".remap", new_file, idx, total, enc_in_filters, enc_ex_filters, key);
				if (err != OK) {
					return err;
				}
			}
		} else {
			//old remap mode, will still work, but it's unused because it's not multiple pck export friendly
			custom_map["path_remap/remapped_paths"] = path_remaps;
		}
	}

	// Store icon and splash images directly, they need to bypass the import system and be loaded as images
	String icon = ProjectSettings::get_singleton()->get("application/config/icon");
	String splash = ProjectSettings::get_singleton()->get("application/boot_splash/image");
	if (!icon.is_empty() && FileAccess::exists(icon)) {
		Vector<uint8_t> array = FileAccess::get_file_as_array(icon);
		err = p_func(p_udata, icon, array, idx, total, enc_in_filters, enc_ex_filters, key);
		if (err != OK) {
			return err;
		}
	}
	if (!splash.is_empty() && FileAccess::exists(splash) && icon != splash) {
		Vector<uint8_t> array = FileAccess::get_file_as_array(splash);
		err = p_func(p_udata, splash, array, idx, total, enc_in_filters, enc_ex_filters, key);
		if (err != OK) {
			return err;
		}
	}
	String resource_cache_file = ResourceUID::get_cache_file();
	if (FileAccess::exists(resource_cache_file)) {
		Vector<uint8_t> array = FileAccess::get_file_as_array(resource_cache_file);
		err = p_func(p_udata, resource_cache_file, array, idx, total, enc_in_filters, enc_ex_filters, key);
		if (err != OK) {
			return err;
		}
	}

	String extension_list_config_file = NativeExtension::get_extension_list_config_file();
	if (FileAccess::exists(extension_list_config_file)) {
		Vector<uint8_t> array = FileAccess::get_file_as_array(extension_list_config_file);
		err = p_func(p_udata, extension_list_config_file, array, idx, total, enc_in_filters, enc_ex_filters, key);
		if (err != OK) {
			return err;
		}
	}

	// Store text server data if it is supported.
	if (TS->has_feature(TextServer::FEATURE_USE_SUPPORT_DATA)) {
		bool use_data = ProjectSettings::get_singleton()->get("internationalization/locale/include_text_server_data");
		if (use_data) {
			// Try using user provided data file.
			String ts_data = "res://" + TS->get_support_data_filename();
			if (FileAccess::exists(ts_data)) {
				Vector<uint8_t> array = FileAccess::get_file_as_array(ts_data);
				err = p_func(p_udata, ts_data, array, idx, total, enc_in_filters, enc_ex_filters, key);
				if (err != OK) {
					return err;
				}
			} else {
				// Use default text server data.
				String icu_data_file = EditorPaths::get_singleton()->get_cache_dir().plus_file("tmp_icu_data");
				TS->save_support_data(icu_data_file);
				Vector<uint8_t> array = FileAccess::get_file_as_array(icu_data_file);
				err = p_func(p_udata, ts_data, array, idx, total, enc_in_filters, enc_ex_filters, key);
				DirAccess::remove_file_or_error(icu_data_file);
				if (err != OK) {
					return err;
				}
			}
		}
	}

	String config_file = "project.binary";
	String engine_cfb = EditorPaths::get_singleton()->get_cache_dir().plus_file("tmp" + config_file);
	ProjectSettings::get_singleton()->save_custom(engine_cfb, custom_map, custom_list);
	Vector<uint8_t> data = FileAccess::get_file_as_array(engine_cfb);
	DirAccess::remove_file_or_error(engine_cfb);

	return p_func(p_udata, "res://" + config_file, data, idx, total, enc_in_filters, enc_ex_filters, key);
}

Error EditorExportPlatform::_add_shared_object(void *p_userdata, const SharedObject &p_so) {
	PackData *pack_data = (PackData *)p_userdata;
	if (pack_data->so_files) {
		pack_data->so_files->push_back(p_so);
	}

	return OK;
}

Error EditorExportPlatform::save_pack(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, Vector<SharedObject> *p_so_files, bool p_embed, int64_t *r_embedded_start, int64_t *r_embedded_size) {
	EditorProgress ep("savepack", TTR("Packing"), 102, true);

	// Create the temporary export directory if it doesn't exist.
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	da->make_dir_recursive(EditorPaths::get_singleton()->get_cache_dir());

	String tmppath = EditorPaths::get_singleton()->get_cache_dir().plus_file("packtmp");
	Ref<FileAccess> ftmp = FileAccess::open(tmppath, FileAccess::WRITE);
	if (ftmp.is_null()) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Save PCK"), vformat(TTR("Cannot create file \"%s\"."), tmppath));
		return ERR_CANT_CREATE;
	}

	PackData pd;
	pd.ep = &ep;
	pd.f = ftmp;
	pd.so_files = p_so_files;

	Error err = export_project_files(p_preset, p_debug, _save_pack_file, &pd, _add_shared_object);

	// Close temp file.
	pd.f.unref();
	ftmp.unref();

	if (err != OK) {
		DirAccess::remove_file_or_error(tmppath);
		add_message(EXPORT_MESSAGE_ERROR, TTR("Save PCK"), TTR("Failed to export project files."));
		return err;
	}

	pd.file_ofs.sort(); //do sort, so we can do binary search later

	Ref<FileAccess> f;
	int64_t embed_pos = 0;
	if (!p_embed) {
		// Regular output to separate PCK file
		f = FileAccess::open(p_path, FileAccess::WRITE);
		if (f.is_null()) {
			DirAccess::remove_file_or_error(tmppath);
			add_message(EXPORT_MESSAGE_ERROR, TTR("Save PCK"), vformat(TTR("Can't open file to read from path \"%s\"."), tmppath));
			return ERR_CANT_CREATE;
		}
	} else {
		// Append to executable
		f = FileAccess::open(p_path, FileAccess::READ_WRITE);
		if (f.is_null()) {
			DirAccess::remove_file_or_error(tmppath);
			add_message(EXPORT_MESSAGE_ERROR, TTR("Save PCK"), vformat(TTR("Can't open executable file from path \"%s\"."), tmppath));
			return ERR_FILE_CANT_OPEN;
		}

		f->seek_end();
		embed_pos = f->get_position();

		if (r_embedded_start) {
			*r_embedded_start = embed_pos;
		}

		// Ensure embedded PCK starts at a 64-bit multiple
		int pad = f->get_position() % 8;
		for (int i = 0; i < pad; i++) {
			f->store_8(0);
		}
	}

	int64_t pck_start_pos = f->get_position();

	f->store_32(PACK_HEADER_MAGIC);
	f->store_32(PACK_FORMAT_VERSION);
	f->store_32(VERSION_MAJOR);
	f->store_32(VERSION_MINOR);
	f->store_32(VERSION_PATCH);

	uint32_t pack_flags = 0;
	bool enc_pck = p_preset->get_enc_pck();
	bool enc_directory = p_preset->get_enc_directory();
	if (enc_pck && enc_directory) {
		pack_flags |= PACK_DIR_ENCRYPTED;
	}
	f->store_32(pack_flags); // flags

	uint64_t file_base_ofs = f->get_position();
	f->store_64(0); // files base

	for (int i = 0; i < 16; i++) {
		//reserved
		f->store_32(0);
	}

	f->store_32(pd.file_ofs.size()); //amount of files

	Ref<FileAccessEncrypted> fae;
	Ref<FileAccess> fhead = f;

	if (enc_pck && enc_directory) {
		String script_key = p_preset->get_script_encryption_key().to_lower();
		Vector<uint8_t> key;
		key.resize(32);
		if (script_key.length() == 64) {
			for (int i = 0; i < 32; i++) {
				int v = 0;
				if (i * 2 < script_key.length()) {
					char32_t ct = script_key[i * 2];
					if (is_digit(ct)) {
						ct = ct - '0';
					} else if (ct >= 'a' && ct <= 'f') {
						ct = 10 + ct - 'a';
					}
					v |= ct << 4;
				}

				if (i * 2 + 1 < script_key.length()) {
					char32_t ct = script_key[i * 2 + 1];
					if (is_digit(ct)) {
						ct = ct - '0';
					} else if (ct >= 'a' && ct <= 'f') {
						ct = 10 + ct - 'a';
					}
					v |= ct;
				}
				key.write[i] = v;
			}
		}
		fae.instantiate();
		if (fae.is_null()) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Save PCK"), TTR("Can't create encrypted file."));
			return ERR_CANT_CREATE;
		}

		err = fae->open_and_parse(f, key, FileAccessEncrypted::MODE_WRITE_AES256, false);
		if (err != OK) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("Save PCK"), TTR("Can't open encrypted file to write."));
			return ERR_CANT_CREATE;
		}

		fhead = fae;
	}

	for (int i = 0; i < pd.file_ofs.size(); i++) {
		uint32_t string_len = pd.file_ofs[i].path_utf8.length();
		uint32_t pad = _get_pad(4, string_len);

		fhead->store_32(string_len + pad);
		fhead->store_buffer((const uint8_t *)pd.file_ofs[i].path_utf8.get_data(), string_len);
		for (uint32_t j = 0; j < pad; j++) {
			fhead->store_8(0);
		}

		fhead->store_64(pd.file_ofs[i].ofs);
		fhead->store_64(pd.file_ofs[i].size); // pay attention here, this is where file is
		fhead->store_buffer(pd.file_ofs[i].md5.ptr(), 16); //also save md5 for file
		uint32_t flags = 0;
		if (pd.file_ofs[i].encrypted) {
			flags |= PACK_FILE_ENCRYPTED;
		}
		fhead->store_32(flags);
	}

	if (fae.is_valid()) {
		fhead.unref();
		fae.unref();
	}

	int header_padding = _get_pad(PCK_PADDING, f->get_position());
	for (int i = 0; i < header_padding; i++) {
		f->store_8(Math::rand() % 256);
	}

	uint64_t file_base = f->get_position();
	f->seek(file_base_ofs);
	f->store_64(file_base); // update files base
	f->seek(file_base);

	// Save the rest of the data.

	ftmp = FileAccess::open(tmppath, FileAccess::READ);
	if (ftmp.is_null()) {
		DirAccess::remove_file_or_error(tmppath);
		add_message(EXPORT_MESSAGE_ERROR, TTR("Save PCK"), vformat(TTR("Can't open file to read from path \"%s\"."), tmppath));
		return ERR_CANT_CREATE;
	}

	const int bufsize = 16384;
	uint8_t buf[bufsize];

	while (true) {
		uint64_t got = ftmp->get_buffer(buf, bufsize);
		if (got == 0) {
			break;
		}
		f->store_buffer(buf, got);
	}

	ftmp.unref(); // Close temp file.

	if (p_embed) {
		// Ensure embedded data ends at a 64-bit multiple
		uint64_t embed_end = f->get_position() - embed_pos + 12;
		uint64_t pad = embed_end % 8;
		for (uint64_t i = 0; i < pad; i++) {
			f->store_8(0);
		}

		uint64_t pck_size = f->get_position() - pck_start_pos;
		f->store_64(pck_size);
		f->store_32(PACK_HEADER_MAGIC);

		if (r_embedded_size) {
			*r_embedded_size = f->get_position() - embed_pos;
		}
	}

	DirAccess::remove_file_or_error(tmppath);

	return OK;
}

Error EditorExportPlatform::save_zip(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path) {
	EditorProgress ep("savezip", TTR("Packing"), 102, true);

	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	zipFile zip = zipOpen2(p_path.utf8().get_data(), APPEND_STATUS_CREATE, nullptr, &io);

	ZipData zd;
	zd.ep = &ep;
	zd.zip = zip;

	Error err = export_project_files(p_preset, p_debug, _save_zip_file, &zd);
	if (err != OK && err != ERR_SKIP) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Save ZIP"), TTR("Failed to export project files."));
	}

	zipClose(zip, nullptr);

	return OK;
}

Error EditorExportPlatform::export_pack(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, int p_flags) {
	ExportNotifier notifier(*this, p_preset, p_debug, p_path, p_flags);
	return save_pack(p_preset, p_debug, p_path);
}

Error EditorExportPlatform::export_zip(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, int p_flags) {
	ExportNotifier notifier(*this, p_preset, p_debug, p_path, p_flags);
	return save_zip(p_preset, p_debug, p_path);
}

void EditorExportPlatform::gen_export_flags(Vector<String> &r_flags, int p_flags) {
	String host = EditorSettings::get_singleton()->get("network/debug/remote_host");
	int remote_port = (int)EditorSettings::get_singleton()->get("network/debug/remote_port");

	if (p_flags & DEBUG_FLAG_REMOTE_DEBUG_LOCALHOST) {
		host = "localhost";
	}

	if (p_flags & DEBUG_FLAG_DUMB_CLIENT) {
		int port = EditorSettings::get_singleton()->get("filesystem/file_server/port");
		String passwd = EditorSettings::get_singleton()->get("filesystem/file_server/password");
		r_flags.push_back("--remote-fs");
		r_flags.push_back(host + ":" + itos(port));
		if (!passwd.is_empty()) {
			r_flags.push_back("--remote-fs-password");
			r_flags.push_back(passwd);
		}
	}

	if (p_flags & DEBUG_FLAG_REMOTE_DEBUG) {
		r_flags.push_back("--remote-debug");

		r_flags.push_back(get_debug_protocol() + host + ":" + String::num(remote_port));

		List<String> breakpoints;
		ScriptEditor::get_singleton()->get_breakpoints(&breakpoints);

		if (breakpoints.size()) {
			r_flags.push_back("--breakpoints");
			String bpoints;
			for (List<String>::Element *E = breakpoints.front(); E; E = E->next()) {
				bpoints += E->get().replace(" ", "%20");
				if (E->next()) {
					bpoints += ",";
				}
			}

			r_flags.push_back(bpoints);
		}
	}

	if (p_flags & DEBUG_FLAG_VIEW_COLLISONS) {
		r_flags.push_back("--debug-collisions");
	}

	if (p_flags & DEBUG_FLAG_VIEW_NAVIGATION) {
		r_flags.push_back("--debug-navigation");
	}
}

EditorExportPlatform::EditorExportPlatform() {
}

////

EditorExport *EditorExport::singleton = nullptr;

void EditorExport::_save() {
	Ref<ConfigFile> config;
	config.instantiate();
	for (int i = 0; i < export_presets.size(); i++) {
		Ref<EditorExportPreset> preset = export_presets[i];
		String section = "preset." + itos(i);

		config->set_value(section, "name", preset->get_name());
		config->set_value(section, "platform", preset->get_platform()->get_name());
		config->set_value(section, "runnable", preset->is_runnable());
		config->set_value(section, "custom_features", preset->get_custom_features());

		bool save_files = false;
		switch (preset->get_export_filter()) {
			case EditorExportPreset::EXPORT_ALL_RESOURCES: {
				config->set_value(section, "export_filter", "all_resources");
			} break;
			case EditorExportPreset::EXPORT_SELECTED_SCENES: {
				config->set_value(section, "export_filter", "scenes");
				save_files = true;
			} break;
			case EditorExportPreset::EXPORT_SELECTED_RESOURCES: {
				config->set_value(section, "export_filter", "resources");
				save_files = true;
			} break;
			case EditorExportPreset::EXCLUDE_SELECTED_RESOURCES: {
				config->set_value(section, "export_filter", "exclude");
				save_files = true;
			} break;
		}

		if (save_files) {
			Vector<String> export_files = preset->get_files_to_export();
			config->set_value(section, "export_files", export_files);
		}
		config->set_value(section, "include_filter", preset->get_include_filter());
		config->set_value(section, "exclude_filter", preset->get_exclude_filter());
		config->set_value(section, "export_path", preset->get_export_path());
		config->set_value(section, "encryption_include_filters", preset->get_enc_in_filter());
		config->set_value(section, "encryption_exclude_filters", preset->get_enc_ex_filter());
		config->set_value(section, "encrypt_pck", preset->get_enc_pck());
		config->set_value(section, "encrypt_directory", preset->get_enc_directory());
		config->set_value(section, "script_export_mode", preset->get_script_export_mode());
		config->set_value(section, "script_encryption_key", preset->get_script_encryption_key());

		String option_section = "preset." + itos(i) + ".options";

		for (const PropertyInfo &E : preset->get_properties()) {
			config->set_value(option_section, E.name, preset->get(E.name));
		}
	}

	config->save("res://export_presets.cfg");
}

void EditorExport::save_presets() {
	if (block_save) {
		return;
	}
	save_timer->start();
}

void EditorExport::_bind_methods() {
	ADD_SIGNAL(MethodInfo("export_presets_updated"));
}

void EditorExport::add_export_platform(const Ref<EditorExportPlatform> &p_platform) {
	export_platforms.push_back(p_platform);
}

int EditorExport::get_export_platform_count() {
	return export_platforms.size();
}

Ref<EditorExportPlatform> EditorExport::get_export_platform(int p_idx) {
	ERR_FAIL_INDEX_V(p_idx, export_platforms.size(), Ref<EditorExportPlatform>());

	return export_platforms[p_idx];
}

void EditorExport::add_export_preset(const Ref<EditorExportPreset> &p_preset, int p_at_pos) {
	if (p_at_pos < 0) {
		export_presets.push_back(p_preset);
	} else {
		export_presets.insert(p_at_pos, p_preset);
	}
}

String EditorExportPlatform::test_etc2() const {
	const bool etc2_supported = ProjectSettings::get_singleton()->get("rendering/textures/vram_compression/import_etc2");

	if (!etc2_supported) {
		return TTR("Target platform requires 'ETC2' texture compression. Enable 'Import Etc 2' in Project Settings.");
	}

	return String();
}

int EditorExport::get_export_preset_count() const {
	return export_presets.size();
}

Ref<EditorExportPreset> EditorExport::get_export_preset(int p_idx) {
	ERR_FAIL_INDEX_V(p_idx, export_presets.size(), Ref<EditorExportPreset>());
	return export_presets[p_idx];
}

void EditorExport::remove_export_preset(int p_idx) {
	export_presets.remove_at(p_idx);
	save_presets();
}

void EditorExport::add_export_plugin(const Ref<EditorExportPlugin> &p_plugin) {
	if (!export_plugins.has(p_plugin)) {
		export_plugins.push_back(p_plugin);
	}
}

void EditorExport::remove_export_plugin(const Ref<EditorExportPlugin> &p_plugin) {
	export_plugins.erase(p_plugin);
}

Vector<Ref<EditorExportPlugin>> EditorExport::get_export_plugins() {
	return export_plugins;
}

void EditorExport::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			load_config();
		} break;

		case NOTIFICATION_PROCESS: {
			update_export_presets();
		} break;
	}
}

void EditorExport::load_config() {
	Ref<ConfigFile> config;
	config.instantiate();
	Error err = config->load("res://export_presets.cfg");
	if (err != OK) {
		return;
	}

	block_save = true;

	int index = 0;
	while (true) {
		String section = "preset." + itos(index);
		if (!config->has_section(section)) {
			break;
		}

		String platform = config->get_value(section, "platform");

		Ref<EditorExportPreset> preset;

		for (int i = 0; i < export_platforms.size(); i++) {
			if (export_platforms[i]->get_name() == platform) {
				preset = export_platforms.write[i]->create_preset();
				break;
			}
		}

		if (!preset.is_valid()) {
			index++;
			ERR_CONTINUE(!preset.is_valid());
		}

		preset->set_name(config->get_value(section, "name"));
		preset->set_runnable(config->get_value(section, "runnable"));

		if (config->has_section_key(section, "custom_features")) {
			preset->set_custom_features(config->get_value(section, "custom_features"));
		}

		String export_filter = config->get_value(section, "export_filter");

		bool get_files = false;

		if (export_filter == "all_resources") {
			preset->set_export_filter(EditorExportPreset::EXPORT_ALL_RESOURCES);
		} else if (export_filter == "scenes") {
			preset->set_export_filter(EditorExportPreset::EXPORT_SELECTED_SCENES);
			get_files = true;
		} else if (export_filter == "resources") {
			preset->set_export_filter(EditorExportPreset::EXPORT_SELECTED_RESOURCES);
			get_files = true;
		} else if (export_filter == "exclude") {
			preset->set_export_filter(EditorExportPreset::EXCLUDE_SELECTED_RESOURCES);
			get_files = true;
		}

		if (get_files) {
			Vector<String> files = config->get_value(section, "export_files");

			for (int i = 0; i < files.size(); i++) {
				if (!FileAccess::exists(files[i])) {
					preset->remove_export_file(files[i]);
				} else {
					preset->add_export_file(files[i]);
				}
			}
		}

		preset->set_include_filter(config->get_value(section, "include_filter"));
		preset->set_exclude_filter(config->get_value(section, "exclude_filter"));
		preset->set_export_path(config->get_value(section, "export_path", ""));

		if (config->has_section_key(section, "encrypt_pck")) {
			preset->set_enc_pck(config->get_value(section, "encrypt_pck"));
		}
		if (config->has_section_key(section, "encrypt_directory")) {
			preset->set_enc_directory(config->get_value(section, "encrypt_directory"));
		}
		if (config->has_section_key(section, "encryption_include_filters")) {
			preset->set_enc_in_filter(config->get_value(section, "encryption_include_filters"));
		}
		if (config->has_section_key(section, "encryption_exclude_filters")) {
			preset->set_enc_ex_filter(config->get_value(section, "encryption_exclude_filters"));
		}
		if (config->has_section_key(section, "script_export_mode")) {
			preset->set_script_export_mode(config->get_value(section, "script_export_mode"));
		}
		if (config->has_section_key(section, "script_encryption_key")) {
			preset->set_script_encryption_key(config->get_value(section, "script_encryption_key"));
		}

		String option_section = "preset." + itos(index) + ".options";

		List<String> options;

		config->get_section_keys(option_section, &options);

		for (const String &E : options) {
			Variant value = config->get_value(option_section, E);

			preset->set(E, value);
		}

		add_export_preset(preset);
		index++;
	}

	block_save = false;
}

void EditorExport::update_export_presets() {
	HashMap<StringName, List<EditorExportPlatform::ExportOption>> platform_options;

	for (int i = 0; i < export_platforms.size(); i++) {
		Ref<EditorExportPlatform> platform = export_platforms[i];

		if (platform->should_update_export_options()) {
			List<EditorExportPlatform::ExportOption> options;
			platform->get_export_options(&options);

			platform_options[platform->get_name()] = options;
		}
	}

	bool export_presets_updated = false;
	for (int i = 0; i < export_presets.size(); i++) {
		Ref<EditorExportPreset> preset = export_presets[i];
		if (platform_options.has(preset->get_platform()->get_name())) {
			export_presets_updated = true;

			List<EditorExportPlatform::ExportOption> options = platform_options[preset->get_platform()->get_name()];

			// Copy the previous preset values
			HashMap<StringName, Variant> previous_values = preset->values;

			// Clear the preset properties and values prior to reloading
			preset->properties.clear();
			preset->values.clear();

			for (const EditorExportPlatform::ExportOption &E : options) {
				preset->properties.push_back(E.option);

				StringName option_name = E.option.name;
				preset->values[option_name] = previous_values.has(option_name) ? previous_values[option_name] : E.default_value;
			}
		}
	}

	if (export_presets_updated) {
		emit_signal(_export_presets_updated);
	}
}

bool EditorExport::poll_export_platforms() {
	bool changed = false;
	for (int i = 0; i < export_platforms.size(); i++) {
		if (export_platforms.write[i]->poll_export()) {
			changed = true;
		}
	}

	return changed;
}

EditorExport::EditorExport() {
	save_timer = memnew(Timer);
	add_child(save_timer);
	save_timer->set_wait_time(0.8);
	save_timer->set_one_shot(true);
	save_timer->connect("timeout", callable_mp(this, &EditorExport::_save));

	_export_presets_updated = "export_presets_updated";

	singleton = this;
	set_process(true);
}

EditorExport::~EditorExport() {
}

//////////

void EditorExportPlatformPC::get_preset_features(const Ref<EditorExportPreset> &p_preset, List<String> *r_features) {
	if (p_preset->get("texture_format/s3tc")) {
		r_features->push_back("s3tc");
	}
	if (p_preset->get("texture_format/etc")) {
		r_features->push_back("etc");
	}
	if (p_preset->get("texture_format/etc2")) {
		r_features->push_back("etc2");
	}

	if (p_preset->get("binary_format/64_bits")) {
		r_features->push_back("64");
	} else {
		r_features->push_back("32");
	}
}

void EditorExportPlatformPC::get_export_options(List<ExportOption> *r_options) {
	String ext_filter = (get_os_name() == "Windows") ? "*.exe" : "";
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "custom_template/debug", PROPERTY_HINT_GLOBAL_FILE, ext_filter), ""));
	r_options->push_back(ExportOption(PropertyInfo(Variant::STRING, "custom_template/release", PROPERTY_HINT_GLOBAL_FILE, ext_filter), ""));

	r_options->push_back(ExportOption(PropertyInfo(Variant::INT, "debug/export_console_script", PROPERTY_HINT_ENUM, "No,Debug Only,Debug and Release"), 1));

	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "binary_format/64_bits"), true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "binary_format/embed_pck"), false));

	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "texture_format/bptc"), false));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "texture_format/s3tc"), true));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "texture_format/etc"), false));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "texture_format/etc2"), false));
	r_options->push_back(ExportOption(PropertyInfo(Variant::BOOL, "texture_format/no_bptc_fallbacks"), true));
}

String EditorExportPlatformPC::get_name() const {
	return name;
}

String EditorExportPlatformPC::get_os_name() const {
	return os_name;
}

Ref<Texture2D> EditorExportPlatformPC::get_logo() const {
	return logo;
}

bool EditorExportPlatformPC::can_export(const Ref<EditorExportPreset> &p_preset, String &r_error, bool &r_missing_templates) const {
	String err;
	bool valid = false;

	// Look for export templates (first official, and if defined custom templates).

	bool use64 = p_preset->get("binary_format/64_bits");
	bool dvalid = exists_export_template(get_template_file_name("debug", use64 ? "64" : "32"), &err);
	bool rvalid = exists_export_template(get_template_file_name("release", use64 ? "64" : "32"), &err);

	if (p_preset->get("custom_template/debug") != "") {
		dvalid = FileAccess::exists(p_preset->get("custom_template/debug"));
		if (!dvalid) {
			err += TTR("Custom debug template not found.") + "\n";
		}
	}
	if (p_preset->get("custom_template/release") != "") {
		rvalid = FileAccess::exists(p_preset->get("custom_template/release"));
		if (!rvalid) {
			err += TTR("Custom release template not found.") + "\n";
		}
	}

	valid = dvalid || rvalid;
	r_missing_templates = !valid;

	if (!err.is_empty()) {
		r_error = err;
	}
	return valid;
}

Error EditorExportPlatformPC::export_project(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, int p_flags) {
	ExportNotifier notifier(*this, p_preset, p_debug, p_path, p_flags);

	Error err = prepare_template(p_preset, p_debug, p_path, p_flags);
	if (err == OK) {
		err = modify_template(p_preset, p_debug, p_path, p_flags);
	}
	if (err == OK) {
		err = export_project_data(p_preset, p_debug, p_path, p_flags);
	}

	return err;
}

Error EditorExportPlatformPC::prepare_template(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, int p_flags) {
	if (!DirAccess::exists(p_path.get_base_dir())) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Prepare Template"), TTR("The given export path doesn't exist."));
		return ERR_FILE_BAD_PATH;
	}

	String custom_debug = p_preset->get("custom_template/debug");
	String custom_release = p_preset->get("custom_template/release");

	String template_path = p_debug ? custom_debug : custom_release;

	template_path = template_path.strip_edges();

	if (template_path.is_empty()) {
		template_path = find_export_template(get_template_file_name(p_debug ? "debug" : "release", p_preset->get("binary_format/64_bits") ? "64" : "32"));
	}

	if (!template_path.is_empty() && !FileAccess::exists(template_path)) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Prepare Template"), vformat(TTR("Template file not found: \"%s\"."), template_path));
		return ERR_FILE_NOT_FOUND;
	}

	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	da->make_dir_recursive(p_path.get_base_dir());
	Error err = da->copy(template_path, p_path, get_chmod_flags());
	if (err != OK) {
		add_message(EXPORT_MESSAGE_ERROR, TTR("Prepare Template"), TTR("Failed to copy export template."));
	}

	return err;
}

Error EditorExportPlatformPC::export_project_data(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, int p_flags) {
	String pck_path;
	if (p_preset->get("binary_format/embed_pck")) {
		pck_path = p_path;
	} else {
		pck_path = p_path.get_basename() + ".pck";
	}

	Vector<SharedObject> so_files;

	int64_t embedded_pos;
	int64_t embedded_size;
	Error err = save_pack(p_preset, p_debug, pck_path, &so_files, p_preset->get("binary_format/embed_pck"), &embedded_pos, &embedded_size);
	if (err == OK && p_preset->get("binary_format/embed_pck")) {
		if (embedded_size >= 0x100000000 && !p_preset->get("binary_format/64_bits")) {
			add_message(EXPORT_MESSAGE_ERROR, TTR("PCK Embedding"), TTR("On 32-bit exports the embedded PCK cannot be bigger than 4 GiB."));
			return ERR_INVALID_PARAMETER;
		}

		err = fixup_embedded_pck(p_path, embedded_pos, embedded_size);
	}

	if (err == OK && !so_files.is_empty()) {
		// If shared object files, copy them.
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		for (int i = 0; i < so_files.size() && err == OK; i++) {
			String src_path = ProjectSettings::get_singleton()->globalize_path(so_files[i].path);
			String target_path;
			if (so_files[i].target.is_empty()) {
				target_path = p_path.get_base_dir().plus_file(src_path.get_file());
			} else {
				target_path = p_path.get_base_dir().plus_file(so_files[i].target).plus_file(src_path.get_file());
			}

			if (da->dir_exists(src_path)) {
				err = da->make_dir_recursive(target_path);
				if (err == OK) {
					err = da->copy_dir(src_path, target_path, -1, true);
				}
			} else {
				err = da->copy(src_path, target_path);
				if (err == OK) {
					err = sign_shared_object(p_preset, p_debug, target_path);
				}
			}
		}
	}

	return err;
}

Error EditorExportPlatformPC::sign_shared_object(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path) {
	return OK;
}

void EditorExportPlatformPC::set_name(const String &p_name) {
	name = p_name;
}

void EditorExportPlatformPC::set_os_name(const String &p_name) {
	os_name = p_name;
}

void EditorExportPlatformPC::set_logo(const Ref<Texture2D> &p_logo) {
	logo = p_logo;
}

void EditorExportPlatformPC::get_platform_features(List<String> *r_features) {
	r_features->push_back("pc"); //all pcs support "pc"
	r_features->push_back("s3tc"); //all pcs support "s3tc" compression
	r_features->push_back(get_os_name().to_lower()); //OS name is a feature
}

void EditorExportPlatformPC::resolve_platform_feature_priorities(const Ref<EditorExportPreset> &p_preset, HashSet<String> &p_features) {
	if (p_features.has("bptc")) {
		if (p_preset->has("texture_format/no_bptc_fallbacks")) {
			p_features.erase("s3tc");
		}
	}
}

int EditorExportPlatformPC::get_chmod_flags() const {
	return chmod_flags;
}

void EditorExportPlatformPC::set_chmod_flags(int p_flags) {
	chmod_flags = p_flags;
}

///////////////////////

void EditorExportTextSceneToBinaryPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	String extension = p_path.get_extension().to_lower();
	if (extension != "tres" && extension != "tscn") {
		return;
	}

	bool convert = GLOBAL_GET("editor/export/convert_text_resources_to_binary");
	if (!convert) {
		return;
	}
	String tmp_path = EditorPaths::get_singleton()->get_cache_dir().plus_file("tmpfile.res");
	Error err = ResourceFormatLoaderText::convert_file_to_binary(p_path, tmp_path);
	if (err != OK) {
		DirAccess::remove_file_or_error(tmp_path);
		ERR_FAIL();
	}
	Vector<uint8_t> data = FileAccess::get_file_as_array(tmp_path);
	if (data.size() == 0) {
		DirAccess::remove_file_or_error(tmp_path);
		ERR_FAIL();
	}
	DirAccess::remove_file_or_error(tmp_path);
	add_file(p_path + ".converted.res", data, true);
}

EditorExportTextSceneToBinaryPlugin::EditorExportTextSceneToBinaryPlugin() {
	GLOBAL_DEF("editor/export/convert_text_resources_to_binary", false);
}
