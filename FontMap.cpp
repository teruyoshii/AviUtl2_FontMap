//----------------------------------------------------------------------------------
//	FontMap for AviUtl ExEdit2
//	フォントをグリフの形が似ているもの同士が近くに来るように2次元の地図に並べ、地図から選んで設定するプラグイン
//
//	コンセプトは木葉はづくさんの Font Map (https://ideoaves.github.io/Font_Map/) によるものです。
//  実装に際しては蛇色さんの フォント視覚化MO (https://github.com/hebiiro/al2_visual_font) を参考にしました。
//
//	- 特徴を取る文字(既定は「AQRwsy アあ令笙」)の輪郭から、FontAnalysis.cppで線の太さ・角・曲線の割合等を測る。
//	- 組の文字が無いフォントはその組を除いて距離を測る。
//	- UMAPで2次元に並べる(Umap.cpp) 。
//	- 解析の結果と地図は、アプリケーションデータのフォルダの Plugin\FontMap に保存される。
//
//	By teruyoshi
//----------------------------------------------------------------------------------
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

#include "FontAnalysis.h"
#include "Umap.h"

using Microsoft::WRL::ComPtr;

LOG_HANDLE* logger = nullptr;
CONFIG_HANDLE* config = nullptr;
EDIT_HANDLE* edit_handle = nullptr;

COMMON_PLUGIN_TABLE common_plugin_table = {
	L"FontMap",
	L"FontMap v1.0 By teruyoshi",
};

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
	logger = handle;
}
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) {
	config = handle;
}
EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
	return &common_plugin_table;
}

#define DIALOG_CLASS_NAME	L"FontMapDialog"
#define CANVAS_CLASS_NAME	L"FontMapCanvas"

#define IDC_PREVIEW		1001
#define IDC_FEATURES	1002
#define IDC_RECALC		1003
#define IDC_ADD			1011
#define IDC_STATUS		1004
#define IDC_NAME		1005
#define IDC_DECIDE		1006
#define IDC_CANCEL		1007
#define IDC_CANVAS		1008
#define IDC_SHOW_MISSING	1009
#define IDC_SHOW_ALL	1010

#define WM_APP_PROGRESS	(WM_APP + 1)	// wparam: 済んだ量, lparam: 全体の量
#define WM_APP_DONE		(WM_APP + 2)

const wchar_t* DEFAULT_FEATURES = L"AQRwsy アあ令笙";
const wchar_t* DEFAULT_PREVIEW = L"Aaあア";
const int DEFAULT_OFFSCREEN_TRANSPARENCY = 70;	// 表示範囲の外のフォントを示す点の透明度(%)。
const int DEFAULT_GLYPH_SIZE = 22;		// 見本の文字サイズ
const int NEIGHBOR_COUNT = 15;			// 選んだ時に近いフォントとして線を繋ぐ件数
const int PREVIEW_MAX_CHARS = 16;		// 見本に使う文字数の最大
const double DRAG_THRESHOLD = 5;  // Alt+ドラッグで点を動かす時にマウスが動いたとみなす距離

//=======================================================================
//	文字列・ファイル
//=======================================================================

std::string to_utf8(const std::wstring& s) {
	if (s.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring from_utf8(const std::string& s) {
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring out(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
	return out;
}

std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
	std::vector<std::wstring> out;
	std::wstring cur;
	for (wchar_t c : s) {
		if (c == sep) { out.push_back(cur); cur.clear(); }
		else cur += c;
	}
	out.push_back(cur);
	return out;
}

// 特徴を取る文字の設定を空白で区切って組に分ける
std::vector<std::wstring> feature_sets(const std::wstring& text) {
	std::vector<std::wstring> sets;
	std::wstring cur;
	for (wchar_t c : text + L" ") {
		if (c == L' ' || c == L'\t' || c == L'\x3000') {
			if (!cur.empty()) sets.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	return sets;
}

// 保存場所は Plugin/FontMap/
std::wstring data_dir() {
	static std::wstring dir;
	if (!dir.empty()) return dir;
	wchar_t path[MAX_PATH] = {};
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&data_dir, &self);
	GetModuleFileNameW(self, path, MAX_PATH);
	std::wstring p = path;
	std::wstring base = p.substr(0, p.find_last_of(L'\\'));
	for (std::wstring cur = base; ; ) {
		size_t slash = cur.find_last_of(L'\\');
		if (slash == std::wstring::npos) break;
		if (_wcsicmp(cur.substr(slash + 1).c_str(), L"Plugin") == 0) { base = cur; break; }
		cur = cur.substr(0, slash);
	}
	dir = base + L"\\FontMap";
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir;
}

std::wstring settings_path() {
	std::wstring path = data_dir() + L"\\settings.ini";
	// UTF-16(BOM付き)で先に作ってエンコードのエラーを防ぐ
	if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
		HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			const BYTE bom[] = { 0xFF, 0xFE };
			DWORD written = 0;
			WriteFile(file, bom, sizeof(bom), &written, nullptr);
			CloseHandle(file);
		}
	}
	return path;
}

std::wstring read_setting(const wchar_t* key, const wchar_t* def) {
	wchar_t buf[1024];
	GetPrivateProfileStringW(L"FontMap", key, def, buf, 1024, settings_path().c_str());
	return buf;
}

int read_setting_int(const wchar_t* key, int def) {
	return GetPrivateProfileIntW(L"FontMap", key, def, settings_path().c_str());
}

void write_setting(const wchar_t* key, const std::wstring& value) {
	// iniの値の前後の空白は読む時に消えるので、""で囲む
	WritePrivateProfileStringW(L"FontMap", key, (L"\"" + value + L"\"").c_str(), settings_path().c_str());
}

void write_setting_int(const wchar_t* key, int value) {
	WritePrivateProfileStringW(L"FontMap", key, std::to_wstring(value).c_str(), settings_path().c_str());
}

std::vector<std::string> read_lines(const std::wstring& path) {
	std::vector<std::string> lines;
	std::ifstream in(path, std::ios::binary);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		lines.push_back(line);
	}
	return lines;
}

//=======================================================================
//	フォント
//=======================================================================

struct Font {
	std::wstring name;		// 本体での登録名
	std::wstring id;		// 名前とファイル(パス・更新日時・番号)。
	ComPtr<IDWriteFontFace> face;
	double upem = 1000, ascent = 800, descent = 200;
};
std::vector<Font> g_fonts;
std::map<std::wstring, int> g_font_index;	// 名前 → g_fontsの番号

// フォントの識別子: 名前 + "|" + ファイルのパス + 更新日時 + "|" + 番号
std::wstring face_file_key(IDWriteFontFace* face) {
	UINT32 count = 0;
	if (FAILED(face->GetFiles(&count, nullptr)) || !count) return L"";
	std::vector<IDWriteFontFile*> files(count);
	if (FAILED(face->GetFiles(&count, files.data()))) return L"";
	std::wstring key;
	const void* ref = nullptr;
	UINT32 ref_size = 0;
	ComPtr<IDWriteFontFileLoader> loader;
	ComPtr<IDWriteLocalFontFileLoader> local;
	if (SUCCEEDED(files[0]->GetReferenceKey(&ref, &ref_size)) && SUCCEEDED(files[0]->GetLoader(&loader)) && SUCCEEDED(loader.As(&local))) {
		UINT32 len = 0;
		if (SUCCEEDED(local->GetFilePathLengthFromKey(ref, ref_size, &len))) {
			std::wstring path(len + 1, L'\0');
			local->GetFilePathFromKey(ref, ref_size, path.data(), len + 1);
			path.resize(len);
			key = path;
		}
		FILETIME time = {};
		if (SUCCEEDED(local->GetLastWriteTimeFromKey(ref, ref_size, &time))) {
			wchar_t buf[32];
			swprintf_s(buf, L"|%08lx%08lx", time.dwHighDateTime, time.dwLowDateTime);
			key += buf;
		}
	}
	for (auto f : files) f->Release();
	key += L"|" + std::to_wstring(face->GetIndex());
	return key;
}

// 本体のフォント一覧を取り直す
void refresh_fonts(EDIT_SECTION* edit) {
	std::vector<std::wstring> names;
	edit_handle->enum_font_name(&names, [](void* param, LPCWSTR name) {
		static_cast<std::vector<std::wstring>*>(param)->push_back(name);
	});
	ULONGLONG start = GetTickCount64();
	int added = 0;
	for (auto& name : names) {
		if (g_font_index.count(name)) continue;
		IDWriteFont* font = edit->get_font(name.c_str());
		Font f;
		f.name = name;
		if (font && SUCCEEDED(font->CreateFontFace(&f.face))) {
			DWRITE_FONT_METRICS m;
			f.face->GetMetrics(&m);
			f.upem = m.designUnitsPerEm;
			f.ascent = m.ascent;
			f.descent = m.descent;
			f.id = name + L"|" + face_file_key(f.face.Get());
		}
		g_font_index[name] = (int)g_fonts.size();
		g_fonts.push_back(f);
		added++;
	}
	if (logger && added) {
		wchar_t msg[128];
		swprintf_s(msg, L"FontMap: フォントを%d件読み込みました (%llums)", added, GetTickCount64() - start);
		logger->info(logger, msg);
	}
}

//=======================================================================
//	保存: 解析の結果 (features.tsv) と地図 (map.tsv)
//=======================================================================

// フォントのid + "\t" + 組の文字 → 結果
std::map<std::wstring, SetResult> g_features;
bool g_features_loaded = false;

// 作った時の特徴を取る文字とフォントのid → 座標
std::wstring g_map_features;
std::map<std::wstring, std::array<double, 2>> g_map;
std::set<std::wstring> g_moved;	// Alt+ドラッグで手で動かしたフォントのid

std::wstring feature_key(const std::wstring& id, const std::wstring& set) {
	return id + L"\t" + set;
}

void load_caches() {
	if (g_features_loaded) return;
	g_features_loaded = true;
	// id \t 組の文字 \t 文字ごとの結果(「-」ならなし、26個の値,…;画像の16進) を \t で区切ったもの
	for (auto& line : read_lines(data_dir() + L"\\features.tsv")) {
		if (line.empty() || line[0] == '#') continue;
		auto cols = split(from_utf8(line), L'\t');
		if (cols.size() < 2) continue;
		SetResult r;
		for (size_t c = 2; c < cols.size(); c++) {
			CharResult ch;
			if (cols[c] != L"-") {
				size_t semi = cols[c].find(L';');
				if (semi == std::wstring::npos) continue;
				auto values = split(cols[c].substr(0, semi), L',');
				std::wstring hex = cols[c].substr(semi + 1);
				if (values.size() != RAW_FEATURES || hex.size() != RASTER_SIZE * RASTER_SIZE) continue;
				for (int k = 0; k < RAW_FEATURES; k++) ch.raw[k] = wcstod(values[k].c_str(), nullptr);
				for (int k = 0; k < RASTER_SIZE * RASTER_SIZE; k++) ch.raster[k] = (uint8_t)wcstol(hex.substr(k, 1).c_str(), nullptr, 16);
				ch.has = true;
				r.missing = false;
			}
			r.chars.push_back(ch);
		}
		g_features[feature_key(cols[0], cols[1])] = r;
	}
	auto map_lines = read_lines(data_dir() + L"\\map.tsv");
	for (size_t i = 0; i < map_lines.size(); i++) {
		auto cols = split(from_utf8(map_lines[i]), L'\t');
		if (i == 0) {
			if (cols.size() < 2 || cols[0] != L"#features") break;
			g_map_features = cols[1];
			continue;
		}
		if (cols.size() < 3) continue;
		g_map[cols[0]] = { wcstod(cols[1].c_str(), nullptr), wcstod(cols[2].c_str(), nullptr) };
		if (cols.size() >= 4 && cols[3] == L"m") g_moved.insert(cols[0]);
	}
}

void save_caches() {
	{
		std::ofstream out(data_dir() + L"\\features.tsv", std::ios::binary | std::ios::trunc);
		out << "# FontMap features v1\n";
		for (auto& [key, r] : g_features) {
			std::wstring line = key;
			for (auto& ch : r.chars) {
				line += L"\t";
				if (!ch.has) { line += L"-"; continue; }
				for (int k = 0; k < RAW_FEATURES; k++) {
					wchar_t buf[32];
					swprintf_s(buf, L"%s%.6g", k ? L"," : L"", ch.raw[k]);
					line += buf;
				}
				line += L";";
				for (uint8_t v : ch.raster) line += L"0123456789abcdef"[v & 15];
			}
			out << to_utf8(line) << "\n";
		}
	}
	{
		std::ofstream out(data_dir() + L"\\map.tsv", std::ios::binary | std::ios::trunc);
		out << to_utf8(L"#features\t" + g_map_features) << "\n";
		for (auto& [id, p] : g_map) {
			wchar_t buf[64];
			swprintf_s(buf, L"\t%.6f\t%.6f%s", p[0], p[1], g_moved.count(id) ? L"\tm" : L"");
			out << to_utf8(id + buf) << "\n";
		}
	}
}

//=======================================================================
//	計算 (別のスレッド)
//=======================================================================

struct Job {
	std::vector<std::wstring> sets;			// 特徴を取る文字の組
	std::vector<int> fonts;					// g_fontsの番号
	bool add_only = false;					// 地図に無いフォントだけドライに追加する
	std::map<std::wstring, std::array<double, 2>> existing;	// add_onlyの時: 計算を始めた時の地図
	std::atomic<bool> cancel{ false };
	std::thread thread;
	// 結果
	std::map<std::wstring, SetResult> new_features;
	std::vector<int> placed;				// 地図に載せたフォント
	std::vector<std::array<double, 2>> positions;
	int failed = 0;
	ULONGLONG analyze_ms = 0, layout_ms = 0;
};


void run_job(Job* job, HWND notify) {
	ULONGLONG start = GetTickCount64();
	std::vector<std::vector<UINT32>> codepoints;
	for (auto& s : job->sets) codepoints.push_back(to_codepoints(s));
	// 解析していないフォントを解析
	size_t n = job->fonts.size();
	for (size_t i = 0; i < n; i++) {
		if (job->cancel) return;
		const Font& f = g_fonts[job->fonts[i]];
		for (size_t s = 0; s < job->sets.size(); s++) {
			std::wstring key = feature_key(f.id, job->sets[s]);
			if (g_features.count(key)) continue;
			job->new_features[key] = analyze_set(f.face.Get(), codepoints[s]);
		}
		if (i % 4 == 0) PostMessageW(notify, WM_APP_PROGRESS, i, n * 2);
	}
	job->analyze_ms = GetTickCount64() - start;
	start = GetTickCount64();
	// 特徴ベクトルを作る (どの組の文字も無いフォントは載せない)
	auto lookup = [&](const std::wstring& key) -> const SetResult* {
		auto it = job->new_features.find(key);
		if (it != job->new_features.end()) return &it->second;
		auto g = g_features.find(key);
		return g != g_features.end() ? &g->second : nullptr;
	};
	std::vector<std::vector<const SetResult*>> rows;
	for (int fi : job->fonts) {
		std::vector<const SetResult*> row;
		bool any = false;
		for (auto& s : job->sets) {
			const SetResult* r = lookup(feature_key(g_fonts[fi].id, s));
			row.push_back(r);
			if (r && !r->missing) any = true;
		}
		if (!any) { job->failed++; continue; }
		job->placed.push_back(fi);
		rows.push_back(row);
	}
	std::vector<int> counts;
	for (auto& c : codepoints) counts.push_back((int)c.size());
	FeatureSpace space = build_feature_space(rows, counts);
	if (job->add_only) {
		// 追加するフォントを、特徴が近い既存のフォント(最大5件)の位置の重み付き平均に置く
		const int K = 5;
		std::vector<int> old_rows;
		for (int k = 0; k < (int)job->placed.size(); k++) if (job->existing.count(g_fonts[job->placed[k]].id)) old_rows.push_back(k);
		std::vector<int> added;
		for (int k = 0; k < (int)job->placed.size() && !old_rows.empty(); k++) {
			if (job->cancel) return;
			const std::wstring& id = g_fonts[job->placed[k]].id;
			if (job->existing.count(id)) continue;
			std::vector<std::pair<double, int>> d;
			for (int o : old_rows) d.push_back({ space.distance(space.vectors[k], space.vectors[o]), o });
			int m = std::min(K, (int)d.size());
			std::partial_sort(d.begin(), d.begin() + m, d.end());
			double x = 0, y = 0, wsum = 0;
			for (int j = 0; j < m; j++) {
				double w = 1 / (d[j].first + 1e-6);
				const auto& pos = job->existing.at(g_fonts[job->placed[d[j].second]].id);
				x += pos[0] * w; y += pos[1] * w; wsum += w;
			}
			// 同じ所に重ならないように少しずらす
			double angle = added.size() * 2.39996, radius = 0.004 * (1 + added.size() % 5);
			job->positions.push_back({ x / wsum + radius * std::cos(angle), y / wsum + radius * std::sin(angle) });
			added.push_back(job->placed[k]);
		}
		job->placed = added;
		job->layout_ms = GetTickCount64() - start;
		if (!job->cancel) PostMessageW(notify, WM_APP_DONE, 0, 0);
		return;
	}
	UmapParams params;
	int count = (int)job->placed.size();
	job->positions = run_umap(count, space.vectors,
		[&](int i, int j) { return space.distance(space.vectors[i], space.vectors[j]); },
		params, job->cancel,
		[&](int done, int total) { PostMessageW(notify, WM_APP_PROGRESS, n + (WPARAM)((double)done / total * n), n * 2); });
	job->layout_ms = GetTickCount64() - start;
	if (!job->cancel) PostMessageW(notify, WM_APP_DONE, 0, 0);
}

//=======================================================================
//	地図の画面
//=======================================================================

ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dwrite;
HFONT g_ui_font = nullptr;
UINT g_dpi = 96;

int scale(int v) {
	return MulDiv(v, g_dpi, 96);
}

// 地図の点1つ
struct Node {
	int font;
	double x, y;
	std::vector<UINT16> glyphs;
	std::vector<float> advances;	// em
	float width = 0;				// em
	bool visible = false;			// 見本の文字が全部あるか
};

struct MapState {
	HWND dialog = nullptr, canvas = nullptr;
	std::wstring current;			// 現在の設定値
	std::wstring preview;
	bool preview_from_text = false;	// 見本をテキストの内容から取ったか
	bool decided = false;
	std::wstring result;

	std::vector<Node> nodes;
	int unplaced = 0;				// 地図に無いフォントの数
	FeatureSpace space;				// nodesと同じ並びの特徴
	std::unique_ptr<Job> job;

	int hover = -1, selected = -1;
	std::vector<int> neighbors;
	double zoom = 1, ox = 0, oy = 0;
	double glyph_size = DEFAULT_GLYPH_SIZE;
	bool dragging = false, drag_moved = false;
	int moving = -1;				// Alt+ドラッグで動かしている点
	D2D1_POINT_2F move_offset{};	// 点の位置 - 押した位置
	bool alt_dragged = false;		// Alt+ドラッグをしたか
	POINT drag_start{};
	double drag_ox = 0, drag_oy = 0;

	const std::set<std::wstring>* allowed = nullptr;	// 地図に載せるフォントの名前
	bool can_show_all = false;		// 「全てのフォントを表示」を出すか
	bool show_all = false;			// allowedに無いフォントも載せるか
	bool show_missing = false;		// 見本の文字が無いフォントも載せるか
	float offscreen_alpha = 0;		// 表示範囲の外のフォントの方向に描く点の不透明度

	// 上下の帯
	int bar_height = 0;
	struct Text { std::wstring text; RECT rect; };
	Text preview_label, features_label, status, name;

	ComPtr<ID2D1HwndRenderTarget> target;
	ComPtr<ID2D1SolidColorBrush> brush;
	ComPtr<IDWriteTextFormat> label_format;
	ComPtr<IDWriteTextFormat> bar_format;	// 帯の文字 (1行、はみ出したら「…」)
};
MapState* g_state = nullptr;

void invalidate_bars() {
	MapState& st = *g_state;
	RECT rc;
	GetClientRect(st.canvas, &rc);
	RECT top = { 0, 0, rc.right, st.bar_height }, bottom = { 0, rc.bottom - st.bar_height, rc.right, rc.bottom };
	InvalidateRect(st.canvas, &top, FALSE);
	InvalidateRect(st.canvas, &bottom, FALSE);
}

void set_status(const std::wstring& text) {
	g_state->status.text = text;
	invalidate_bars();
}

void update_name_label() {
	MapState& st = *g_state;
	int i = st.selected >= 0 ? st.selected : st.hover;
	std::wstring text = L"今のフォント: " + st.current;
	if (i >= 0) text = (st.selected >= 0 ? L"選択: " : L"") + g_fonts[st.nodes[i].font].name;
	st.name.text = text;
	invalidate_bars();
	EnableWindow(GetDlgItem(st.dialog, IDC_DECIDE), st.selected >= 0);
}

// 見本の文字のグリフを取り直す。見本の文字が無いフォントは隠す
void update_glyphs() {
	MapState& st = *g_state;
	auto cps = to_codepoints(st.preview);
	cps.erase(std::remove_if(cps.begin(), cps.end(), [](UINT32 c) { return c < 0x20; }), cps.end());
	for (auto& node : st.nodes) {
		const Font& f = g_fonts[node.font];
		node.glyphs.assign(cps.size(), 0);
		node.advances.assign(cps.size(), 0);
		node.width = 0;
		node.visible = !cps.empty() && SUCCEEDED(f.face->GetGlyphIndices(cps.data(), (UINT32)cps.size(), node.glyphs.data()));
		if (!node.visible) continue;
		std::vector<DWRITE_GLYPH_METRICS> metrics(cps.size());
		f.face->GetDesignGlyphMetrics(node.glyphs.data(), (UINT32)cps.size(), metrics.data(), FALSE);
		for (size_t k = 0; k < cps.size(); k++) {
			if (!node.glyphs[k] && cps[k] != L' ' && cps[k] != 0x3000 && !st.show_missing) node.visible = false;
			node.advances[k] = (float)(metrics[k].advanceWidth / f.upem);
			node.width += node.advances[k];
		}
	}
	if (st.selected >= 0 && !st.nodes[st.selected].visible) {
		st.selected = -1;
		st.neighbors.clear();
	}
	st.hover = -1;
}

void update_status() {
	MapState& st = *g_state;
	int shown = (int)std::count_if(st.nodes.begin(), st.nodes.end(), [](const Node& n) { return n.visible; });
	std::wstring status = std::to_wstring(st.nodes.size()) + L"件";
	if (shown < (int)st.nodes.size()) status += L" (見本の文字が無いフォント " + std::to_wstring(st.nodes.size() - shown) + L"件を隠しています)";
	if (st.unplaced) status += L" (地図に無いフォント " + std::to_wstring(st.unplaced) + L"件。「追加」で追加)";
	if (!g_map_features.empty() && g_map_features != read_setting(L"features", DEFAULT_FEATURES)) status += L" (特徴を取る文字が変わっています。「再計算」で反映)";
	set_status(status);
}

// 地図に載せるフォントを保存した地図から作り直す
void rebuild_nodes() {
	MapState& st = *g_state;
	st.nodes.clear();
	st.selected = st.hover = -1;
	st.neighbors.clear();
	std::vector<std::vector<const SetResult*>> rows;
	st.unplaced = 0;
	auto sets = feature_sets(g_map_features);
	for (int i = 0; i < (int)g_fonts.size(); i++) {
		const Font& f = g_fonts[i];
		if (!f.face) continue;
		if (st.allowed && !(st.can_show_all && st.show_all) && !st.allowed->count(f.name)) continue;
		std::vector<const SetResult*> row;
		bool analyzed = true, any = false;
		for (auto& s : sets) {
			auto r = g_features.find(feature_key(f.id, s));
			row.push_back(r != g_features.end() ? &r->second : nullptr);
			if (r == g_features.end()) analyzed = false;
			else if (!r->second.missing) any = true;
		}
		auto it = g_map.find(f.id);
		if (it == g_map.end()) {
			// 解析して特徴を取る文字が1つも無かったフォントは数えない
			if (!analyzed || any || sets.empty()) st.unplaced++;
			continue;
		}
		st.nodes.push_back({ i, it->second[0], it->second[1] });
		rows.push_back(row);
	}
	std::vector<int> counts;
	for (auto& s : sets) counts.push_back((int)to_codepoints(s).size());
	st.space = build_feature_space(rows, counts);
	update_glyphs();
	update_status();
	update_name_label();
}

// add_only: 地図に無いフォントを今の地図を崩さずドライに足す
void start_job(bool add_only = false) {
	MapState& st = *g_state;
	if (st.job) return;
	if (add_only && (g_map.empty() || feature_sets(g_map_features).empty())) add_only = false;
	std::vector<std::wstring> sets;
	if (add_only) {
		sets = feature_sets(g_map_features);
	} else {
		wchar_t buf[512];
		GetDlgItemTextW(st.dialog, IDC_FEATURES, buf, 512);
		std::wstring features = buf;
		sets = feature_sets(features);
		if (sets.empty()) {
			MessageBoxW(st.dialog, L"特徴を取る文字を入力してください。", L"FontMap", MB_ICONWARNING);
			return;
		}
		int moved = 0;
		for (auto& id : g_moved) if (g_map.count(id)) moved++;
		if (moved) {
			std::wstring text = L"手で動かしたフォントが " + std::to_wstring(moved) + L"件あります。\n再計算すると、動かした位置は元に戻ります。再計算しますか?";
			if (MessageBoxW(st.dialog, text.c_str(), L"FontMap", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
		}
		write_setting(L"features", features);
	}
	st.job = std::make_unique<Job>();
	st.job->sets = sets;
	st.job->add_only = add_only;
	if (add_only) st.job->existing = g_map;
	for (int i = 0; i < (int)g_fonts.size(); i++) if (g_fonts[i].face) st.job->fonts.push_back(i);
	EnableWindow(GetDlgItem(st.dialog, IDC_ADD), FALSE);
	EnableWindow(GetDlgItem(st.dialog, IDC_RECALC), FALSE);
	EnableWindow(GetDlgItem(st.dialog, IDC_FEATURES), FALSE);
	set_status(L"計算中...");
	Job* job = st.job.get();
	HWND dialog = st.dialog;
	job->thread = std::thread([job, dialog] { run_job(job, dialog); });
}

void stop_job() {
	MapState& st = *g_state;
	if (!st.job) return;
	st.job->cancel = true;
	if (st.job->thread.joinable()) st.job->thread.join();
	// 途中までの解析結果は残す
	for (auto& [key, r] : st.job->new_features) g_features[key] = r;
	if (!st.job->new_features.empty()) save_caches();
	st.job.reset();
}

void finish_job() {
	MapState& st = *g_state;
	Job& job = *st.job;
	if (job.thread.joinable()) job.thread.join();
	for (auto& [key, r] : job.new_features) g_features[key] = r;
	std::wstring features;
	for (auto& s : job.sets) features += (features.empty() ? L"" : L" ") + s;
	bool add_only = job.add_only;
	if (add_only) {
		for (size_t i = 0; i < job.placed.size(); i++) g_map[g_fonts[job.placed[i]].id] = job.positions[i];
	} else if (!job.positions.empty()) {
		g_map.clear();
		g_moved.clear();
		g_map_features = features;
		for (size_t i = 0; i < job.placed.size(); i++) g_map[g_fonts[job.placed[i]].id] = job.positions[i];
	}
	save_caches();
	if (logger) {
		wchar_t msg[256];
		swprintf_s(msg, L"FontMap: 解析 %llums、配置 %llums (地図に載せたフォント %zu件、文字が無く載せなかったフォント %d件)",
			job.analyze_ms, job.layout_ms, job.placed.size(), job.failed);
		logger->info(logger, msg);
	}
	bool too_few = job.positions.empty();
	size_t added = job.placed.size();
	st.job.reset();
	EnableWindow(GetDlgItem(st.dialog, IDC_ADD), TRUE);
	EnableWindow(GetDlgItem(st.dialog, IDC_RECALC), TRUE);
	EnableWindow(GetDlgItem(st.dialog, IDC_FEATURES), TRUE);
	if (!add_only) {
		// 並びが変わるので拡大・移動を戻す
		st.zoom = 1;
		st.ox = st.oy = 0;
	}
	rebuild_nodes();
	if (add_only) set_status(added ? std::to_wstring(added) + L"件を地図に追加しました" : L"追加するフォントはありませんでした");
	else if (too_few) set_status(L"地図を作れませんでした (特徴を取る文字があるフォントが3件未満)");
	InvalidateRect(st.canvas, nullptr, FALSE);
}

//-----------------------------------------------------------------------
//	描画・操作
//-----------------------------------------------------------------------

D2D1_POINT_2F to_screen(const Node& n) {
	RECT rc;
	GetClientRect(g_state->canvas, &rc);
	double w = rc.right, h = rc.bottom, r = std::min(w, h) * 0.45 * g_state->zoom;
	return { (float)(w / 2 + g_state->ox + n.x * r), (float)(h / 2 + g_state->oy + n.y * r) };
}

double glyph_px() {
	return g_state->glyph_size * g_dpi / 96.0;
}

// 点の見本の文字が占める範囲
D2D1_RECT_F node_rect(const Node& n) {
	auto p = to_screen(n);
	float half_w = (float)(n.width * glyph_px() / 2), half_h = (float)(glyph_px() * 0.65);
	return { p.x - half_w, p.y - half_h, p.x + half_w, p.y + half_h };
}

int pick(int mx, int my) {
	int best = -1;
	double best_d = INFINITY;
	for (int i = 0; i < (int)g_state->nodes.size(); i++) {
		const Node& n = g_state->nodes[i];
		if (!n.visible) continue;
		auto r = node_rect(n);
		if (mx < r.left || mx > r.right || my < r.top || my > r.bottom) continue;
		double d = std::pow((r.left + r.right) / 2 - mx, 2) + std::pow((r.top + r.bottom) / 2 - my, 2);
		if (d < best_d) { best_d = d; best = i; }
	}
	return best;
}

void select_node(int i) {
	MapState& st = *g_state;
	st.selected = i;
	st.neighbors.clear();
	if (i >= 0 && i < (int)st.space.vectors.size()) {
		std::vector<std::pair<double, int>> d;
		for (int j = 0; j < (int)st.nodes.size(); j++) {
			if (j == i || !st.nodes[j].visible) continue;
			d.push_back({ st.space.distance(st.space.vectors[i], st.space.vectors[j]), j });
		}
		std::sort(d.begin(), d.end());
		for (int k = 0; k < (int)d.size() && k < NEIGHBOR_COUNT; k++) st.neighbors.push_back(d[k].second);
	}
	update_name_label();
	InvalidateRect(st.canvas, nullptr, FALSE);
}

void draw_label(const std::wstring& text, D2D1_RECT_F anchor, D2D1_SIZE_F size) {
	MapState& st = *g_state;
	ComPtr<IDWriteTextLayout> layout;
	if (FAILED(g_dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), st.label_format.Get(), 1000, 100, &layout))) return;
	DWRITE_TEXT_METRICS m;
	layout->GetMetrics(&m);
	float pad = (float)scale(4);
	float x = std::min(anchor.right + pad, size.width - m.width - pad * 3);
	float y = std::min(anchor.bottom + pad, size.height - m.height - pad * 3);
	x = std::max(x, pad);
	y = std::max(y, pad);
	st.brush->SetColor(D2D1::ColorF(0x000000, 0.8f));
	st.target->FillRectangle({ x, y, x + m.width + pad * 2, y + m.height + pad * 2 }, st.brush.Get());
	st.brush->SetColor(D2D1::ColorF(0xffffff));
	st.target->DrawTextLayout({ x + pad, y + pad }, layout.Get(), st.brush.Get());
}

void paint() {
	MapState& st = *g_state;
	RECT rc;
	GetClientRect(st.canvas, &rc);
	if (!st.target) {
		D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
		props.dpiX = props.dpiY = 96;
		if (FAILED(g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(st.canvas, D2D1::SizeU(rc.right, rc.bottom)), &st.target))) return;
		st.target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &st.brush);
		st.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
	}
	D2D1_SIZE_F size = st.target->GetSize();
	st.target->BeginDraw();
	st.target->Clear(D2D1::ColorF(0x1e1e1e));
	if (st.selected >= 0) {
		auto c = to_screen(st.nodes[st.selected]);
		st.brush->SetColor(D2D1::ColorF(0x808080, 0.8f));
		for (int j : st.neighbors) st.target->DrawLine(c, to_screen(st.nodes[j]), st.brush.Get(), 1);
	}
	std::set<int> close_set(st.neighbors.begin(), st.neighbors.end());
	double px = glyph_px();
	for (int i = 0; i < (int)st.nodes.size(); i++) {
		const Node& n = st.nodes[i];
		if (!n.visible) continue;
		auto r = node_rect(n);
		if (r.right < 0 || r.bottom < 0 || r.left > size.width || r.top > size.height) continue;
		const Font& f = g_fonts[n.font];
		bool is_current = f.name == st.current;
		float alpha = st.selected >= 0 && i != st.selected && !close_set.count(i) ? 0.2f : 0.85f;
		std::vector<float> advances(n.advances.size());
		for (size_t k = 0; k < advances.size(); k++) advances[k] = (float)(n.advances[k] * px);
		DWRITE_GLYPH_RUN run = {};
		run.fontFace = f.face.Get();
		run.fontEmSize = (float)px;
		run.glyphCount = (UINT32)n.glyphs.size();
		run.glyphIndices = n.glyphs.data();
		run.glyphAdvances = advances.data();
		auto c = to_screen(n);
		float baseline = (float)(c.y + (f.ascent - f.descent) / f.upem * px / 2);
		UINT32 color = i == st.selected ? 0x7fd0ff : is_current ? 0xffd070 : 0xe8e8e8;
		st.brush->SetColor(D2D1::ColorF(color, i == st.selected ? 1.0f : alpha));
		st.target->DrawGlyphRun({ r.left, baseline }, &run, st.brush.Get());
		if (is_current || i == st.selected) {
			st.brush->SetColor(D2D1::ColorF(color, 0.9f));
			st.target->DrawRectangle(r, st.brush.Get(), 1.5f);
		}
	}
	if (st.hover >= 0) {
		st.brush->SetColor(D2D1::ColorF(0xffffff));
		st.target->DrawRectangle(node_rect(st.nodes[st.hover]), st.brush.Get(), 1);
	}
	// 表示範囲の外にあるフォントはその方向の地図の端に点を描く
	if (st.offscreen_alpha > 0) {
		float margin = (float)scale(6), radius = (float)scale(3);
		D2D1_RECT_F inner = { margin, (float)st.bar_height + margin, size.width - margin, size.height - st.bar_height - margin };
		if (inner.right > inner.left && inner.bottom > inner.top) {
			float cx = (inner.left + inner.right) / 2, cy = (inner.top + inner.bottom) / 2;
			float hw = (inner.right - inner.left) / 2, hh = (inner.bottom - inner.top) / 2;
			st.brush->SetColor(D2D1::ColorF(0xe8e8e8, st.offscreen_alpha));
			for (const Node& n : st.nodes) {
				if (!n.visible) continue;
				auto p = to_screen(n);
				if (p.x >= inner.left && p.x <= inner.right && p.y >= inner.top && p.y <= inner.bottom) continue;
				float dx = p.x - cx, dy = p.y - cy;
				float t = std::min(dx ? hw / std::abs(dx) : INFINITY, dy ? hh / std::abs(dy) : INFINITY);
				st.target->FillEllipse(D2D1::Ellipse({ cx + dx * t, cy + dy * t }, radius, radius), st.brush.Get());
			}
		}
	}
	// 上下の帯は半透明で塗る
	st.brush->SetColor(D2D1::ColorF(0x1e1e1e, 0.6f));
	float bar = (float)st.bar_height;
	st.target->FillRectangle({ 0, 0, size.width, bar }, st.brush.Get());
	st.target->FillRectangle({ 0, size.height - bar, size.width, size.height }, st.brush.Get());
	st.brush->SetColor(D2D1::ColorF(0xe0e0e0));
	for (auto* t : { &st.preview_label, &st.features_label, &st.status, &st.name }) {
		if (t->text.empty() || t->rect.right <= t->rect.left) continue;
		D2D1_RECT_F r = { (float)t->rect.left, (float)t->rect.top, (float)t->rect.right, (float)t->rect.bottom };
		st.target->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
		st.target->DrawText(t->text.c_str(), (UINT32)t->text.size(), st.bar_format.Get(), r, st.brush.Get());
		st.target->PopAxisAlignedClip();
	}
	int label = st.hover >= 0 ? st.hover : st.selected;
	if (label >= 0) draw_label(g_fonts[st.nodes[label].font].name, node_rect(st.nodes[label]), size);
	if (st.target->EndDraw() == D2DERR_RECREATE_TARGET) {
		st.brush.Reset();
		st.target.Reset();
	}
}

void decide() {
	MapState& st = *g_state;
	if (st.selected < 0) return;
	st.decided = true;
	st.result = g_fonts[st.nodes[st.selected].font].name;
	DestroyWindow(st.dialog);
}

LRESULT CALLBACK canvas_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	MapState* st = g_state;
	switch (message) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		BeginPaint(hwnd, &ps);
		if (st) paint();
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_SIZE:
		if (st && st->target) st->target->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_LBUTTONDOWN:
		SetFocus(hwnd);
		SetCapture(hwnd);
		if (GetKeyState(VK_MENU) < 0) {
			// Alt+ドラッグで点を動かす
			int i = pick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
			if (i >= 0) {
				auto p = to_screen(st->nodes[i]);
				st->moving = i;
				st->drag_moved = false;
				st->drag_start = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
				st->move_offset = { p.x - GET_X_LPARAM(lparam), p.y - GET_Y_LPARAM(lparam) };
				st->alt_dragged = true;
				return 0;
			}
		}
		st->dragging = true;
		st->drag_moved = false;
		st->drag_start = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
		st->drag_ox = st->ox;
		st->drag_oy = st->oy;
		return 0;
	case WM_MOUSEMOVE: {
		int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
		if (st->moving >= 0) {
			if (std::hypot(x - st->drag_start.x, y - st->drag_start.y) > DRAG_THRESHOLD * g_dpi / 96) st->drag_moved = true;
			if (st->drag_moved) {
				RECT rc;
				GetClientRect(hwnd, &rc);
				double r = std::min((double)rc.right, (double)rc.bottom) * 0.45 * st->zoom;
				Node& n = st->nodes[st->moving];
				n.x = (x + st->move_offset.x - rc.right / 2.0 - st->ox) / r;
				n.y = (y + st->move_offset.y - rc.bottom / 2.0 - st->oy) / r;
				SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if (st->dragging) {
			if (std::hypot(x - st->drag_start.x, y - st->drag_start.y) > DRAG_THRESHOLD * g_dpi / 96) st->drag_moved = true;
			if (st->drag_moved) {
				st->ox = st->drag_ox + x - st->drag_start.x;
				st->oy = st->drag_oy + y - st->drag_start.y;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			return 0;
		}
		TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
		TrackMouseEvent(&tme);
		int h = pick(x, y);
		if (h != st->hover) {
			st->hover = h;
			update_name_label();
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		if (st->hover >= 0) {
			st->hover = -1;
			update_name_label();
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_LBUTTONUP:
		if (st->moving >= 0) {
			ReleaseCapture();
			int i = st->moving;
			st->moving = -1;
			if (st->drag_moved) {
				const Node& n = st->nodes[i];
				const std::wstring& id = g_fonts[n.font].id;
				g_map[id] = { n.x, n.y };
				g_moved.insert(id);
				save_caches();
			} else {
				select_node(i);
			}
			return 0;
		}
		if (st->dragging) {
			ReleaseCapture();
			st->dragging = false;
			if (!st->drag_moved) select_node(pick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)));
		}
		return 0;
	case WM_LBUTTONDBLCLK: {
		int i = pick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		if (i >= 0) {
			select_node(i);
			decide();
		}
		return 0;
	}
	case WM_MOUSEWHEEL: {
		POINT p = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
		ScreenToClient(hwnd, &p);
		int delta = GET_WHEEL_DELTA_WPARAM(wparam);
		if (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) {
			// Ctrl+ホイールで見本の文字の大きさを変える
			st->glyph_size = std::max(8.0, std::min(96.0, st->glyph_size * std::exp(delta * 0.0015)));
		} else {
			RECT rc;
			GetClientRect(hwnd, &rc);
			double k = std::exp(delta * 0.0015);
			double cx = p.x - rc.right / 2.0, cy = p.y - rc.bottom / 2.0;
			st->ox = cx - (cx - st->ox) * k;
			st->oy = cy - (cy - st->oy) * k;
			st->zoom *= k;
		}
		st->hover = pick(p.x, p.y);
		update_name_label();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND add_control(HWND parent, LPCWSTR cls, LPCWSTR text, DWORD style, int id) {
	HWND c = CreateWindowExW(wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
		0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
	SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, FALSE);
	return c;
}

// 地図を領域全体に広げ、上下の帯の入力欄・ボタンを重ねる
void layout_dialog(HWND dialog) {
	MapState* st = g_state;
	if (!st) return;
	RECT rc;
	GetClientRect(dialog, &rc);
	int m = scale(8), h = scale(24), bar = h + m * 2, x = m;
	st->bar_height = bar;
	auto label = [&](MapState::Text& t, int w) {
		t.rect = { x, m, x + w, m + h };
		x += w + m;
	};
	auto place = [&](int id, int w) {
		MoveWindow(GetDlgItem(dialog, id), x, m, w, h, TRUE);
		x += w + m;
	};
	label(st->preview_label, scale(36));
	place(IDC_PREVIEW, scale(160));
	label(st->features_label, scale(96));
	place(IDC_FEATURES, scale(180));
	place(IDC_ADD, scale(56));
	place(IDC_RECALC, scale(72));
	st->status.rect = { x, m, std::max(x, (int)rc.right - m), m + h };
	MoveWindow(st->canvas, 0, 0, rc.right, rc.bottom, TRUE);
	int bw = scale(88), by = rc.bottom - bar + m;
	int cw_missing = scale(220), cw_all = st->can_show_all ? scale(150) : 0;
	int cx = rc.right - bw * 2 - m * 3 - cw_missing - (cw_all ? cw_all + m : 0);
	MoveWindow(GetDlgItem(dialog, IDC_SHOW_MISSING), cx, by, cw_missing, h, TRUE);
	if (cw_all) MoveWindow(GetDlgItem(dialog, IDC_SHOW_ALL), cx + cw_missing + m, by, cw_all, h, TRUE);
	st->name.rect = { m, by, std::max(m, cx - m), by + h };
	MoveWindow(GetDlgItem(dialog, IDC_DECIDE), rc.right - bw * 2 - m * 2, by, bw, h, TRUE);
	MoveWindow(GetDlgItem(dialog, IDC_CANCEL), rc.right - bw - m, by, bw, h, TRUE);
	InvalidateRect(st->canvas, nullptr, FALSE);
}

LRESULT CALLBACK dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	MapState* st = g_state;
	switch (message) {
	case WM_SIZE:
		layout_dialog(hwnd);
		return 0;
	case WM_GETMINMAXINFO:
		((MINMAXINFO*)lparam)->ptMinTrackSize = { scale(640), scale(360) };
		return 0;
	case WM_COMMAND:
		switch (LOWORD(wparam)) {
		case IDC_PREVIEW:
			if (HIWORD(wparam) == EN_CHANGE && st) {
				wchar_t buf[256];
				GetDlgItemTextW(hwnd, IDC_PREVIEW, buf, 256);
				st->preview = buf;
				update_glyphs();
				if (!st->job) update_status();
				update_name_label();
				InvalidateRect(st->canvas, nullptr, FALSE);
			}
			return 0;
		case IDC_RECALC:
			start_job();
			return 0;
		case IDC_ADD:
			start_job(true);
			return 0;
		case IDC_SHOW_MISSING:
		case IDC_SHOW_ALL:
			if (HIWORD(wparam) == BN_CLICKED && st) {
				bool on = IsDlgButtonChecked(hwnd, LOWORD(wparam)) == BST_CHECKED;
				if (LOWORD(wparam) == IDC_SHOW_MISSING) {
					st->show_missing = on;
					write_setting_int(L"show_missing", on);
					update_glyphs();
				} else {
					st->show_all = on;
					write_setting_int(L"menu_show_all", on);
					rebuild_nodes();
				}
				if (!st->job) update_status();
				update_name_label();
				InvalidateRect(st->canvas, nullptr, FALSE);
			}
			return 0;
		case IDC_DECIDE:
		case IDOK:
			// Enter: 特徴を取る文字の欄では再計算、それ以外では決定として扱う
			if (LOWORD(wparam) == IDOK && GetFocus() == GetDlgItem(hwnd, IDC_FEATURES)) start_job();
			else decide();
			return 0;
		case IDC_CANCEL:
		case IDCANCEL:
			DestroyWindow(hwnd);
			return 0;
		}
		break;
	case WM_APP_PROGRESS:
		if (st && st->job && lparam) {
			wchar_t buf[128];
			bool layout = wparam >= (WPARAM)lparam / 2;
			swprintf_s(buf, L"%s %d%%", layout ? L"配置を計算中..." : L"フォントを解析中...", (int)(wparam * 100 / lparam));
			set_status(buf);
		}
		return 0;
	case WM_APP_DONE:
		if (st && st->job) finish_job();
		return 0;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY: {
		// 閉じた/最大化した時の位置・大きさを保持する。
		WINDOWPLACEMENT wp = { sizeof(wp) };
		if (GetWindowPlacement(hwnd, &wp)) {
			const RECT& r = wp.rcNormalPosition;
			write_setting_int(L"window_x", r.left);
			write_setting_int(L"window_y", r.top);
			write_setting_int(L"window_w", r.right - r.left);
			write_setting_int(L"window_h", r.bottom - r.top);
			write_setting_int(L"window_maximized", wp.showCmd == SW_SHOWMAXIMIZED || IsZoomed(hwnd));
		}
		break;
	}
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

// 前に閉じた時の位置・大きさ。無い時・どの画面とも重ならない時はfalse
bool saved_placement(WINDOWPLACEMENT& wp) {
	int w = read_setting_int(L"window_w", 0), h = read_setting_int(L"window_h", 0);
	if (w <= 0 || h <= 0) return false;
	RECT r = { read_setting_int(L"window_x", 0), read_setting_int(L"window_y", 0) };
	r.right = r.left + w;
	r.bottom = r.top + h;
	if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return false;
	wp = { sizeof(wp) };
	wp.rcNormalPosition = r;
	wp.showCmd = read_setting_int(L"window_maximized", 0) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
	return true;
}

// 地図を開く時に渡すもの
struct MapRequest {
	std::wstring current;						// 今の設定値
	std::wstring preview;
	bool preview_from_text = false;
	const std::set<std::wstring>* allowed = nullptr;	// 地図に載せるフォント (nullptrなら全部)
	bool can_show_all = false;
};

// 地図の画面を開き、閉じられるまで待つ。選ばれたフォント名を返す (選ばれなければ空)
std::wstring show_map(HWND owner, const MapRequest& req) {
	if (!g_d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
	if (!g_dwrite) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)g_dwrite.GetAddressOf());
	g_dpi = owner ? GetDpiForWindow(owner) : 96;
	if (g_ui_font) DeleteObject(g_ui_font);
	NONCLIENTMETRICSW metrics = { sizeof(metrics) };
	SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, g_dpi);
	g_ui_font = CreateFontIndirectW(&metrics.lfMessageFont);

	HINSTANCE inst = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc = { sizeof(wc) };
	if (!GetClassInfoExW(inst, DIALOG_CLASS_NAME, &wc)) {
		wc.lpfnWndProc = dialog_proc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
		wc.lpszClassName = DIALOG_CLASS_NAME;
		RegisterClassExW(&wc);
		WNDCLASSEXW cc = { sizeof(cc) };
		cc.style = CS_DBLCLKS;
		cc.lpfnWndProc = canvas_proc;
		cc.hInstance = inst;
		cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		cc.lpszClassName = CANVAS_CLASS_NAME;
		RegisterClassExW(&cc);
	}

	MapState st;
	g_state = &st;
	st.current = req.current;
	st.preview = req.preview;
	st.preview_from_text = req.preview_from_text;
	st.allowed = req.allowed;
	st.can_show_all = req.can_show_all && req.allowed;
	st.show_all = read_setting_int(L"menu_show_all", 0) != 0;
	st.show_missing = read_setting_int(L"show_missing", 0) != 0;
	st.offscreen_alpha = 1.0f - std::max(0, std::min(100, read_setting_int(L"offscreen_transparency", DEFAULT_OFFSCREEN_TRANSPARENCY))) / 100.0f;
	st.glyph_size = read_setting_int(L"glyph_size", DEFAULT_GLYPH_SIZE);
	// 前に閉じた時の地図の拡大率と移動量
	st.zoom = wcstod(read_setting(L"view_zoom", L"1").c_str(), nullptr);
	if (!(st.zoom > 0) || !std::isfinite(st.zoom)) st.zoom = 1;
	st.ox = wcstod(read_setting(L"view_x", L"0").c_str(), nullptr);
	st.oy = wcstod(read_setting(L"view_y", L"0").c_str(), nullptr);
	if (!std::isfinite(st.ox) || !std::isfinite(st.oy)) st.ox = st.oy = 0;
	st.preview_label.text = L"見本:";
	st.features_label.text = L"特徴を取る文字:";
	g_dwrite->CreateTextFormat(metrics.lfMessageFont.lfFaceName, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL, (float)scale(13), L"ja-jp", &st.label_format);
	g_dwrite->CreateTextFormat(metrics.lfMessageFont.lfFaceName, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL, (float)scale(13), L"ja-jp", &st.bar_format);
	if (st.bar_format) {
		st.bar_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		st.bar_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
		ComPtr<IDWriteInlineObject> ellipsis;
		g_dwrite->CreateEllipsisTrimmingSign(st.bar_format.Get(), &ellipsis);
		st.bar_format->SetTrimming(&trimming, ellipsis.Get());
	}

	int w = scale(read_setting_int(L"width", 960)), h = scale(read_setting_int(L"height", 680));
	RECT owner_rect = {};
	if (owner) GetWindowRect(owner, &owner_rect);
	int x = owner ? (owner_rect.left + owner_rect.right - w) / 2 : CW_USEDEFAULT;
	int y = owner ? (owner_rect.top + owner_rect.bottom - h) / 2 : CW_USEDEFAULT;
	st.dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, DIALOG_CLASS_NAME, L"フォントマップ",
		WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN, x, y, w, h, owner, nullptr, inst, nullptr);
	HWND d = st.dialog;
	st.canvas = CreateWindowExW(0, CANVAS_CLASS_NAME, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, d, (HMENU)IDC_CANVAS, inst, nullptr);
	add_control(d, L"EDIT", req.preview.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IDC_PREVIEW);
	add_control(d, L"EDIT", read_setting(L"features", DEFAULT_FEATURES).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IDC_FEATURES);
	add_control(d, L"BUTTON", L"追加", WS_TABSTOP, IDC_ADD);
	add_control(d, L"BUTTON", L"再計算", WS_TABSTOP, IDC_RECALC);
	add_control(d, L"BUTTON", L"見本の文字が無いフォントも表示", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_SHOW_MISSING);
	CheckDlgButton(d, IDC_SHOW_MISSING, st.show_missing ? BST_CHECKED : BST_UNCHECKED);
	if (st.can_show_all) {
		add_control(d, L"BUTTON", L"全てのフォントを表示", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_SHOW_ALL);
		CheckDlgButton(d, IDC_SHOW_ALL, st.show_all ? BST_CHECKED : BST_UNCHECKED);
	}
	add_control(d, L"BUTTON", L"決定", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_DECIDE);
	add_control(d, L"BUTTON", L"キャンセル", WS_TABSTOP, IDC_CANCEL);
	SetWindowPos(st.canvas, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	layout_dialog(d);

	load_caches();
	rebuild_nodes();
	WINDOWPLACEMENT placement;
	if (saved_placement(placement)) SetWindowPlacement(d, &placement);
	else ShowWindow(d, SW_SHOW);
	SetFocus(st.canvas);
	// 地図がまだ無ければ計算する
	if (g_map.empty()) start_job();

	if (owner) EnableWindow(owner, FALSE);
	MSG msg;
	while (IsWindow(d) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { DestroyWindow(d); continue; }
		// Alt+ドラッグの後にAltを離しても、ウィンドウのメニューに入らないようにする
		if (msg.message == WM_SYSKEYUP && msg.wParam == VK_MENU && st.alt_dragged) { st.alt_dragged = false; continue; }
		if (IsDialogMessageW(d, &msg)) continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (owner) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}
	stop_job();
	write_setting_int(L"glyph_size", (int)std::lround(st.glyph_size));
	wchar_t view[64];
	swprintf_s(view, L"%.6g", st.zoom);
	write_setting(L"view_zoom", view);
	swprintf_s(view, L"%.1f", st.ox);
	write_setting(L"view_x", view);
	swprintf_s(view, L"%.1f", st.oy);
	write_setting(L"view_y", view);
	g_state = nullptr;
	return st.decided ? st.result : L"";
}

//=======================================================================
//	設定
//=======================================================================

// フォントメニュー・フォントリストボックスを地図に置き換える条件
// (修飾キーを押している時だけにすると、al2_visual_font等のメニュー・一覧のプレビューと使い分けられる)
enum MapMode { MAP_OFF, MAP_ALWAYS, MAP_SHIFT, MAP_CTRL, MAP_ALT, MAP_MODE_COUNT };
const wchar_t* MAP_MODE_NAMES[] = {
	L"マップを表示しない", L"クリックでマップを表示", L"Shift+クリックでマップを表示", L"Ctrl+クリックでマップを表示", L"Alt+クリックでマップを表示",
};

// 本体がクリックを処理している時に呼ぶ (GetKeyStateはそのクリックの時のキーの状態)
bool map_mode_active(int mode) {
	switch (mode) {
	case MAP_ALWAYS: return GetKeyState(VK_SHIFT) >= 0;	// Shift+クリックの時は元のメニュー・一覧
	case MAP_SHIFT: return GetKeyState(VK_SHIFT) < 0;
	case MAP_CTRL: return GetKeyState(VK_CONTROL) < 0;
	case MAP_ALT: return GetKeyState(VK_MENU) < 0;
	}
	return false;
}

// 地図を開く場所ごとの設定
struct Options {
	bool item_text = true;			// 設定項目の右クリック: 見本をテキストの内容に合わせる
	int menu_mode = MAP_ALWAYS;		// フォントメニュー(「フォント」ボタン)
	bool menu_text = true;
	int listbox_mode = MAP_OFF;		// フォントリストボックス(フォント名の▼)
	bool listbox_text = true;
};
Options g_options;

void load_options() {
	auto mode = [](const wchar_t* key, int def) { int v = read_setting_int(key, def); return v >= 0 && v < MAP_MODE_COUNT ? v : def; };
	g_options.item_text = read_setting_int(L"item_text", 1) != 0;
	g_options.menu_mode = mode(L"menu_mode", MAP_ALWAYS);
	g_options.menu_text = read_setting_int(L"menu_text", 1) != 0;
	g_options.listbox_mode = mode(L"listbox_mode", MAP_OFF);
	g_options.listbox_text = read_setting_int(L"listbox_text", 1) != 0;
}

void save_options() {
	write_setting_int(L"item_text", g_options.item_text);
	write_setting_int(L"menu_mode", g_options.menu_mode);
	write_setting_int(L"menu_text", g_options.menu_text);
	write_setting_int(L"listbox_mode", g_options.listbox_mode);
	write_setting_int(L"listbox_text", g_options.listbox_text);
}

#define IDC_OPT_ITEM_TEXT		2001
#define IDC_OPT_MENU_MODE		2002
#define IDC_OPT_MENU_TEXT		2003
#define IDC_OPT_LISTBOX_MODE	2004
#define IDC_OPT_LISTBOX_TEXT	2005
#define IDC_OPT_OK				2006
#define IDC_OPT_CANCEL			2007
#define IDC_OPT_PREVIEW			2008
#define IDC_OPT_OFFSCREEN		2009

LRESULT CALLBACK config_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	switch (message) {
	case WM_COMMAND:
		switch (LOWORD(wparam)) {
		case IDC_OPT_OK:
		case IDOK: {
			auto checked = [&](int id) { return IsDlgButtonChecked(hwnd, id) == BST_CHECKED; };
			auto selected = [&](int id) { int v = (int)SendDlgItemMessageW(hwnd, id, CB_GETCURSEL, 0, 0); return v >= 0 ? v : MAP_OFF; };
			g_options.item_text = checked(IDC_OPT_ITEM_TEXT);
			g_options.menu_mode = selected(IDC_OPT_MENU_MODE);
			g_options.menu_text = checked(IDC_OPT_MENU_TEXT);
			g_options.listbox_mode = selected(IDC_OPT_LISTBOX_MODE);
			g_options.listbox_text = checked(IDC_OPT_LISTBOX_TEXT);
			wchar_t preview[256] = {};
			GetDlgItemTextW(hwnd, IDC_OPT_PREVIEW, preview, 256);
			write_setting(L"preview", preview[0] ? preview : DEFAULT_PREVIEW);
			BOOL ok = FALSE;
			int transparency = (int)GetDlgItemInt(hwnd, IDC_OPT_OFFSCREEN, &ok, FALSE);
			write_setting_int(L"offscreen_transparency", ok ? std::min(100, transparency) : DEFAULT_OFFSCREEN_TRANSPARENCY);
			save_options();
			DestroyWindow(hwnd);
			return 0;
		}
		case IDC_OPT_CANCEL:
		case IDCANCEL:
			DestroyWindow(hwnd);
			return 0;
		}
		break;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

// 「設定」→「プラグイン設定」→「FontMapの設定」
void on_config(HWND owner, HINSTANCE) {
	g_dpi = owner ? GetDpiForWindow(owner) : 96;
	if (g_ui_font) DeleteObject(g_ui_font);
	NONCLIENTMETRICSW metrics = { sizeof(metrics) };
	SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, g_dpi);
	g_ui_font = CreateFontIndirectW(&metrics.lfMessageFont);
	HINSTANCE inst = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc = { sizeof(wc) };
	if (!GetClassInfoExW(inst, L"FontMapConfig", &wc)) {
		wc.lpfnWndProc = config_proc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
		wc.lpszClassName = L"FontMapConfig";
		RegisterClassExW(&wc);
	}
	const int width = 440, height = 416;
	RECT rect = { 0, 0, scale(width), scale(height) };
	DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
	AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_DLGMODALFRAME, g_dpi);
	RECT owner_rect = {};
	if (owner) GetWindowRect(owner, &owner_rect);
	int ww = rect.right - rect.left, wh = rect.bottom - rect.top;
	HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FontMapConfig", L"FontMapの設定", style,
		(owner_rect.left + owner_rect.right - ww) / 2, (owner_rect.top + owner_rect.bottom - wh) / 2, ww, wh, owner, nullptr, inst, nullptr);
	if (!dialog) return;
	auto control = [&](LPCWSTR cls, LPCWSTR text, DWORD s, int x, int y, int w, int h, int id) {
		HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | s, scale(x), scale(y), scale(w), scale(h), dialog, (HMENU)(INT_PTR)id, inst, nullptr);
		SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, FALSE);
		return c;
	};
	auto check = [&](LPCWSTR text, int x, int y, int id, bool on) {
		control(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, width - x - 16, 22, id);
		CheckDlgButton(dialog, id, on ? BST_CHECKED : BST_UNCHECKED);
	};
	auto combo = [&](int x, int y, int id, int mode) {
		HWND c = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, width - x - 16, 200, id);
		for (auto* name : MAP_MODE_NAMES) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)name);
		SendMessageW(c, CB_SETCURSEL, mode, 0);
	};
	int y = 12;
	control(L"STATIC", L"設定項目の右クリック > プラグイン > フォントマップから選ぶ", SS_LEFT, 14, y, width - 28, 20, 0);
	check(L"見本の文字をテキストの内容に合わせる", 30, y += 24, IDC_OPT_ITEM_TEXT, g_options.item_text);
	control(L"STATIC", L"フォントメニュー (オブジェクト設定の「フォント」ボタン)", SS_LEFT, 14, y += 36, width - 28, 20, 0);
	combo(30, y += 24, IDC_OPT_MENU_MODE, g_options.menu_mode);
	check(L"見本の文字をテキストの内容に合わせる", 30, y += 30, IDC_OPT_MENU_TEXT, g_options.menu_text);
	control(L"STATIC", L"フォントリストボックス (フォント名の ▼)", SS_LEFT, 14, y += 36, width - 28, 20, 0);
	combo(30, y += 24, IDC_OPT_LISTBOX_MODE, g_options.listbox_mode);
	check(L"見本の文字をテキストの内容に合わせる", 30, y += 30, IDC_OPT_LISTBOX_TEXT, g_options.listbox_text);
	control(L"STATIC", L"テキストの内容が無い時の見本の文字", SS_LEFT, 14, y += 36, width - 28, 20, 0);
	HWND preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", read_setting(L"preview", DEFAULT_PREVIEW).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
		scale(30), scale(y += 24), scale(width - 46), scale(24), dialog, (HMENU)IDC_OPT_PREVIEW, inst, nullptr);
	SendMessageW(preview, WM_SETFONT, (WPARAM)g_ui_font, FALSE);
	control(L"STATIC", L"表示範囲外のフォントを示す点の透明度(%)", SS_LEFT, 14, y += 36, width - 100, 20, 0);
	HWND offscreen = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(read_setting_int(L"offscreen_transparency", DEFAULT_OFFSCREEN_TRANSPARENCY)).c_str(),
		WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_RIGHT | WS_TABSTOP, scale(width - 76), scale(y - 3), scale(60), scale(24), dialog, (HMENU)IDC_OPT_OFFSCREEN, inst, nullptr);
	SendMessageW(offscreen, EM_LIMITTEXT, 3, 0);
	SendMessageW(offscreen, WM_SETFONT, (WPARAM)g_ui_font, FALSE);
	control(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, width - 196, height - 38, 88, 26, IDC_OPT_OK);
	control(L"BUTTON", L"キャンセル", WS_TABSTOP, width - 102, height - 38, 88, 26, IDC_OPT_CANCEL);
	ShowWindow(dialog, SW_SHOW);
	if (owner) EnableWindow(owner, FALSE);
	MSG msg;
	while (IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (IsDialogMessageW(dialog, &msg)) continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (owner) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}
}

//=======================================================================
//	見本の文字・今のフォント
//=======================================================================

// テキストの項目の内容から見本の文字を作る (1行目の、制御文字<...>を除いた先頭の数文字)
std::wstring preview_from_text(const std::wstring& text) {
	std::wstring line = text.substr(0, text.find_first_of(L"\r\n"));
	std::wstring out;
	bool in_tag = false;
	for (wchar_t c : line) {
		if (c == L'<') in_tag = true;
		else if (c == L'>' && in_tag) in_tag = false;
		else if (!in_tag) out += c;
	}
	size_t a = out.find_first_not_of(L" \t\x3000");
	if (a == std::wstring::npos) return L"";
	out = out.substr(a, out.find_last_not_of(L" \t\x3000") - a + 1);
	auto cps = to_codepoints(out);
	if (cps.size() > PREVIEW_MAX_CHARS) {
		size_t len = 0;
		for (int k = 0; k < PREVIEW_MAX_CHARS; k++) len += cps[k] >= 0x10000 ? 2 : 1;
		out = out.substr(0, len);
	}
	return out;
}

// 見本を決める。use_textでテキストの内容が使えればそれを、使えなければ前に入力した見本
void choose_preview(MapRequest& req, bool use_text, const std::wstring& text) {
	req.preview = use_text ? preview_from_text(text) : L"";
	req.preview_from_text = !req.preview.empty();
	if (!req.preview_from_text) req.preview = read_setting(L"preview", DEFAULT_PREVIEW);
}

// 選択中のオブジェクトのテキストとフォント (フォントメニュー・フォントリストボックスから開く時に使う)
struct FocusContext {
	std::wstring text, font;
};

void read_focus_context(FocusContext& ctx) {
	edit_handle->call_read_section_param(&ctx, [](void* param, EDIT_SECTION* edit) {
		try {
			auto* c = static_cast<FocusContext*>(param);
			refresh_fonts(edit);
			OBJECT_HANDLE object = edit->get_focus_object();
			if (!object) return;
			if (LPCSTR t = edit->get_object_item_value(object, L"テキスト", L"テキスト")) c->text = from_utf8(t);
			if (LPCSTR f = edit->get_object_item_value(object, L"テキスト", L"フォント")) c->font = from_utf8(f);
		} catch (...) {
		}
	});
}

//=======================================================================
//	設定項目の右クリックのメニュー
//=======================================================================

struct ItemContext {
	OBJECT_HANDLE object;
	std::wstring effect, item, text_item;
	std::wstring current, text;
	std::wstring chosen;
	bool written = false;
};

void on_item_menu(void*, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item) {
	HWND owner = edit_handle->get_host_app_window();
	if (!effect || !item || g_state) return;
	ItemContext ctx;
	ctx.object = object;
	ctx.effect = effect;
	ctx.item = item;
	// 項目の種類を確かめる。同じエフェクトのテキストの項目があれば見本に使う
	std::wstring base = ctx.effect.substr(0, ctx.effect.rfind(L':'));
	struct Enum { std::wstring item; int type = 0; std::wstring text_item; } en{ item };
	edit_handle->enum_effect_item(base.c_str(), &en, [](void* param, LPCWSTR name, int type) {
		auto* e = static_cast<Enum*>(param);
		if (e->item == name) e->type = type;
		if (type == EDIT_HANDLE::EFFECT_ITEM_TYPE_TEXT && e->text_item.empty()) e->text_item = name;
	});
	if (en.type != EDIT_HANDLE::EFFECT_ITEM_TYPE_FONT) {
		MessageBoxW(owner, L"フォントの項目を右クリックして選んでください。", L"FontMap", MB_ICONINFORMATION);
		return;
	}
	ctx.text_item = en.text_item;
	edit_handle->call_read_section_param(&ctx, [](void* param, EDIT_SECTION* edit) {
		try {
			auto* c = static_cast<ItemContext*>(param);
			LPCSTR v = edit->get_object_item_value(c->object, c->effect.c_str(), c->item.c_str());
			if (v) c->current = from_utf8(v);
			if (!c->text_item.empty()) {
				LPCSTR t = edit->get_object_item_value(c->object, c->effect.c_str(), c->text_item.c_str());
				if (t) c->text = from_utf8(t);
			}
			refresh_fonts(edit);
		} catch (...) {
		}
	});
	MapRequest req;
	req.current = ctx.current;
	choose_preview(req, g_options.item_text, ctx.text);
	ctx.chosen = show_map(owner, req);
	if (ctx.chosen.empty() || ctx.chosen == ctx.current) return;
	bool called = edit_handle->call_edit_section_param(&ctx, [](void* param, EDIT_SECTION* edit) {
		auto* c = static_cast<ItemContext*>(param);
		try {
			c->written = edit->set_object_item_value(c->object, c->effect.c_str(), c->item.c_str(), to_utf8(c->chosen).c_str());
		} catch (...) {
			c->written = false;
		}
	});
	if (!called || !ctx.written) {
		if (logger) logger->warn(logger, (L"FontMap: フォントを設定できませんでした (" + ctx.chosen + L")").c_str());
		MessageBoxW(owner, L"フォントを設定できませんでした。", L"FontMap", MB_ICONWARNING);
	}
}

//=======================================================================
//	フォントメニュー (オブジェクト設定の「フォント」ボタン) の横取り
//
//	本体はメニューをTrackPopupMenu(TPM_RETURNCMD)で出し、選ばれた項目のIDで設定する。
//	本体の取り込み表(IAT)のTrackPopupMenuを差し替え、フォントのメニューなら代わりに地図を開いて、
//	選ばれたフォントの項目のIDを返す(設定とUndoは本体が行う)。フォントのメニューでなければ元の関数へ渡す
//	(al2_visual_font等がTrackPopupMenu自体をフックしていても、元の関数として呼ばれるので両立する)
//=======================================================================

using TrackPopupMenuProc = BOOL(WINAPI*)(HMENU, UINT, int, int, int, HWND, const RECT*);
TrackPopupMenuProc g_orig_track_popup_menu = nullptr;
std::atomic<bool> g_ready{ false };

// モジュールの取り込み表の、dllのfuncの所をhookに差し替える。差し替える前の値を返す
void* patch_import(HMODULE module, const char* dll, const char* func, void* hook) {
	auto base = (BYTE*)module;
	auto dos = (IMAGE_DOS_HEADER*)base;
	auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) return nullptr;
	for (auto desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); desc->Name; desc++) {
		if (_stricmp((const char*)(base + desc->Name), dll) != 0 || !desc->OriginalFirstThunk) continue;
		auto names = (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk);
		auto funcs = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
		for (; names->u1.AddressOfData; names++, funcs++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
			auto by_name = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
			if (strcmp((const char*)by_name->Name, func) != 0) continue;
			void* old = (void*)funcs->u1.Function;
			DWORD protect = 0;
			VirtualProtect(&funcs->u1.Function, sizeof(void*), PAGE_READWRITE, &protect);
			funcs->u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&funcs->u1.Function, sizeof(void*), protect, &protect);
			return old;
		}
	}
	return nullptr;
}

std::set<std::wstring> font_name_set() {
	std::set<std::wstring> names;
	edit_handle->enum_font_name(&names, [](void* param, LPCWSTR name) {
		static_cast<std::set<std::wstring>*>(param)->insert(name);
	});
	return names;
}

std::wstring menu_item_text(HMENU menu, int pos) {
	wchar_t buf[256] = {};
	GetMenuStringW(menu, pos, buf, 256, MF_BYPOSITION);
	return buf;
}

// メニューのフォント名の項目を集める。名前 → ID、チェックの付いた項目の名前
void collect_menu_fonts(HMENU menu, const std::set<std::wstring>& names, std::map<std::wstring, UINT>& ids, std::wstring& checked) {
	int count = GetMenuItemCount(menu);
	for (int i = 0; i < count; i++) {
		if (HMENU sub = GetSubMenu(menu, i)) {
			collect_menu_fonts(sub, names, ids, checked);
			continue;
		}
		std::wstring text = menu_item_text(menu, i);
		if (!names.count(text)) continue;
		UINT id = GetMenuItemID(menu, i);
		if (id == (UINT)-1 || ids.count(text)) continue;
		ids[text] = id;
		if (GetMenuState(menu, i, MF_BYPOSITION) & MF_CHECKED) checked = text;
	}
}

// メニューに無いフォントが選ばれた時、TrackPopupMenuから戻った後で書き込む
#define WM_APP_WRITE_FONT	(WM_APP + 13)	// lparam: new std::pair<今の値, 選ばれたフォント>
extern HWND g_hwnd;

BOOL WINAPI hook_track_popup_menu(HMENU menu, UINT flags, int x, int y, int reserved, HWND hwnd, const RECT* rect) {
	if (g_ready && !g_state && (flags & TPM_RETURNCMD) && map_mode_active(g_options.menu_mode)) {
		std::set<std::wstring> names = font_name_set();
		std::map<std::wstring, UINT> ids;
		std::wstring checked;
		collect_menu_fonts(menu, names, ids, checked);
		// フォント名が項目の半分以上あるメニューはフォントのメニューとみなす
		if (!ids.empty() && (int)ids.size() * 2 >= GetMenuItemCount(menu)) {
			FocusContext focus;
			read_focus_context(focus);
			std::set<std::wstring> allowed;
			for (auto& [name, id] : ids) allowed.insert(name);
			MapRequest req;
			req.current = !checked.empty() ? checked : focus.font;
			req.allowed = &allowed;
			req.can_show_all = true;
			choose_preview(req, g_options.menu_text, focus.text);
			std::wstring chosen = show_map(edit_handle->get_host_app_window(), req);
			auto it = ids.find(chosen);
			if (it != ids.end()) return (BOOL)it->second;
			// 「全てのフォントを表示」でメニューに無いフォントが選ばれた
			if (!chosen.empty() && chosen != req.current) PostMessageW(g_hwnd, WM_APP_WRITE_FONT, 0, (LPARAM)new std::pair<std::wstring, std::wstring>(req.current, chosen));
			return 0;
		}
	}
	return g_orig_track_popup_menu(menu, flags, x, y, reserved, hwnd, rect);
}

//=======================================================================
//	フォントリストボックス (フォント名の ▼) の横取り
//
//	▼で開く一覧は、メインウィンドウが持ち主のポップアップのListBox(使い回される)。項目をクリックすると本体が設定して閉じ、
//	Escで何もせずに閉じる。本体は、一覧が非アクティブになると一覧を隠して中身を消す(一覧へのメッセージを止めても防げない)。
//	そこで、一覧が表示されたら(WH_CALLWNDPROCRETでWM_SHOWWINDOWを見る)、一覧を画面の外へ動かし、▼が押された位置
//	(WH_GETMESSAGEで見た直前の左ボタンのメッセージ)を覚えて一覧をEscで閉じ、地図を開く。フォントが選ばれたら
//	- 選択中のオブジェクトに、一覧の今の値と同じ値のフォントの項目が1つだけあれば、その項目に直接書き込む
//	- そうでなければ、覚えた位置へもう一度クリックを送って一覧を開き直させ、その項目をクリックしたことにする
//=======================================================================

#define WM_APP_LISTBOX			(WM_APP + 10)	// wparam: 一覧のウィンドウ (地図を開く)
#define WM_APP_READY			(WM_APP + 11)
#define WM_APP_LISTBOX_SELECT	(WM_APP + 12)	// wparam: 開き直した一覧のウィンドウ (選んだ項目をクリックする)
const UINT_PTR TIMER_READY = 1;
const UINT_PTR TIMER_REOPEN = 2;
const DWORD READY_SETTLE_MS = 300;
const UINT REOPEN_TIMEOUT_MS = 500;	// 開き直しのクリックを送ってから、一覧が開くのを待つ時間

HWND g_hwnd = nullptr;			// メッセージ専用ウィンドウ
HHOOK g_cwp_hook = nullptr;
ULONGLONG g_ready_since = 0;

// ▼が押された所 (一覧が表示された時の、最後のマウスの位置)
struct ListboxOpener {
	HWND target = nullptr;		// その位置のウィンドウ
	POINT client{};				// targetの中での位置
};
ListboxOpener g_opener;
bool g_listbox_busy = false;	// 地図を開いている間
std::wstring g_reopen_choice;	// 開き直している間の、選ばれたフォント (空なら開き直していない)

std::wstring window_class(HWND hwnd) {
	wchar_t buf[64] = {};
	GetClassNameW(hwnd, buf, 64);
	return buf;
}

std::wstring listbox_text(HWND listbox, int i) {
	int len = (int)SendMessageW(listbox, LB_GETTEXTLEN, i, 0);
	if (len <= 0) return L"";
	std::wstring text(len + 1, L'\0');
	SendMessageW(listbox, LB_GETTEXT, i, (LPARAM)text.data());
	text.resize(len);
	return text;
}

void click_window(HWND target, POINT client) {
	LPARAM pos = MAKELPARAM(client.x, client.y);
	PostMessageW(target, WM_MOUSEMOVE, 0, pos);
	PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, pos);
	PostMessageW(target, WM_LBUTTONUP, 0, pos);
}

// 項目がフォント名の一覧かを先頭の数件で見分ける
bool is_font_listbox(HWND listbox) {
	int count = (int)SendMessageW(listbox, LB_GETCOUNT, 0, 0);
	if (count <= 0) return false;
	std::set<std::wstring> names = font_name_set();
	int fonts = 0, checks = std::min(count, 4);
	for (int i = 0; i < checks; i++) if (names.count(listbox_text(listbox, i))) fonts++;
	return fonts * 2 >= checks;
}

// 選択中のオブジェクトの、値がvalueのフォントの項目
struct FontItem {
	EFFECT_HANDLE effect;
	std::wstring item;
};

std::vector<FontItem> find_font_items(const std::wstring& value) {
	struct Find { std::string value; std::vector<FontItem> found; } find{ to_utf8(value) };
	edit_handle->call_read_section_param(&find, [](void* param, EDIT_SECTION* edit) {
		auto* f = static_cast<Find*>(param);
		try {
			OBJECT_HANDLE object = edit->get_focus_object();
			if (!object) return;
			int n = edit->get_effect_list(object, nullptr, 0);
			std::vector<EFFECT_HANDLE> effects(n);
			n = edit->get_effect_list(object, effects.data(), n);
			for (int i = 0; i < n; i++) {
				std::vector<std::wstring> items;
				edit_handle->enum_effect_item(edit->get_effect_name(effects[i]), &items, [](void* p, LPCWSTR name, int type) {
					if (type == EDIT_HANDLE::EFFECT_ITEM_TYPE_FONT) static_cast<std::vector<std::wstring>*>(p)->push_back(name);
				});
				for (auto& item : items) {
					LPCSTR v = edit->get_effect_item_value(effects[i], item.c_str());
					if (v && f->value == v) f->found.push_back({ effects[i], item });
				}
			}
		} catch (...) {
			f->found.clear();
		}
	});
	return find.found;
}

// 選択中のオブジェクトの値がcurrentのフォントの項目が1つだけならchosenを書き込む
bool write_font(const std::wstring& current, const std::wstring& chosen) {
	std::vector<FontItem> items = find_font_items(current);
	if (items.size() != 1) return false;
	struct Write { FontItem item; std::string value; bool written = false; } write{ items[0], to_utf8(chosen) };
	bool called = edit_handle->call_edit_section_param(&write, [](void* param, EDIT_SECTION* edit) {
		auto* w = static_cast<Write*>(param);
		try {
			w->written = edit->set_effect_item_value(w->item.effect, w->item.item.c_str(), w->value.c_str());
		} catch (...) {
			w->written = false;
		}
	});
	return called && write.written;
}

void open_map_for_listbox(HWND listbox) {
	if (!IsWindow(listbox) || !IsWindowVisible(listbox) || g_state || g_listbox_busy) return;
	int count = (int)SendMessageW(listbox, LB_GETCOUNT, 0, 0);
	std::set<std::wstring> allowed;
	for (int i = 0; i < count; i++) allowed.insert(listbox_text(listbox, i));
	int cur = (int)SendMessageW(listbox, LB_GETCURSEL, 0, 0);
	ListboxOpener opener = g_opener;
	SendMessageW(listbox, WM_KEYDOWN, VK_ESCAPE, 0);

	FocusContext focus;
	read_focus_context(focus);
	MapRequest req;
	req.current = cur >= 0 ? listbox_text(listbox, cur) : focus.font;
	req.allowed = &allowed;
	choose_preview(req, g_options.listbox_text, focus.text);
	g_listbox_busy = true;
	std::wstring chosen = show_map(edit_handle->get_host_app_window(), req);
	g_listbox_busy = false;
	if (chosen.empty() || chosen == req.current) return;
	// 一覧の今の値の項目が選択中のオブジェクトに1つだけあれば、その項目に直接書き込む
	if (cur >= 0 && write_font(req.current, chosen)) return;
	if (!IsWindow(opener.target)) {
		if (logger) logger->warn(logger, L"FontMap: ▼の位置が分からないため一覧を開き直せませんでした");
		return;
	}
	// 一覧を開き直させる。開いたらcwp_hookからWM_APP_LISTBOX_SELECTが来る
	g_reopen_choice = chosen;
	SetTimer(g_hwnd, TIMER_REOPEN, REOPEN_TIMEOUT_MS, nullptr);
	click_window(opener.target, opener.client);
}

// 開き直した一覧で、選ばれたフォントの項目をクリックしたことにする
void select_in_listbox(HWND listbox) {
	KillTimer(g_hwnd, TIMER_REOPEN);
	std::wstring chosen = g_reopen_choice;
	g_reopen_choice.clear();
	if (chosen.empty() || !IsWindow(listbox) || !IsWindowVisible(listbox)) return;
	int index = (int)SendMessageW(listbox, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)chosen.c_str());
	if (index == LB_ERR) {
		SendMessageW(listbox, WM_KEYDOWN, VK_ESCAPE, 0);
		if (logger) logger->warn(logger, (L"FontMap: 開き直した一覧に選んだフォントがありませんでした (" + chosen + L")").c_str());
		return;
	}
	SendMessageW(listbox, LB_SETTOPINDEX, index, 0);
	SendMessageW(listbox, LB_SETCURSEL, index, 0);
	RECT item = {};
	SendMessageW(listbox, LB_GETITEMRECT, index, (LPARAM)&item);
	LPARAM pos = MAKELPARAM(item.left + 4, (item.top + item.bottom) / 2);
	SendMessageW(listbox, WM_LBUTTONDOWN, MK_LBUTTON, pos);
	SendMessageW(listbox, WM_LBUTTONUP, 0, pos);
}

// メインスレッドで最後に取り出された左ボタンのメッセージ (宛先とその中の位置)。
// GetMessagePosは本物のカーソルの位置なので、送られたクリックでは合わない。メッセージのlParamを使う
ListboxOpener g_last_click;
HHOOK g_getmsg_hook = nullptr;

LRESULT CALLBACK getmsg_hook(int code, WPARAM wparam, LPARAM lparam) {
	if (code == HC_ACTION && wparam == PM_REMOVE) {
		auto* msg = (MSG*)lparam;
		if (msg->message == WM_LBUTTONDOWN || msg->message == WM_LBUTTONUP) {
			g_last_click.target = msg->hwnd;
			g_last_click.client = { GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
		}
	}
	return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK cwp_hook(int code, WPARAM wparam, LPARAM lparam) {
	if (code == HC_ACTION && g_ready && !g_state && !g_listbox_busy) {
		auto* cs = (CWPRETSTRUCT*)lparam;
		if (cs->message == WM_SHOWWINDOW && cs->wParam && window_class(cs->hwnd) == L"ListBox"
			&& GetWindow(cs->hwnd, GW_OWNER) == edit_handle->get_host_app_window()) {
			if (!g_reopen_choice.empty()) {
				PostMessageW(g_hwnd, WM_APP_LISTBOX_SELECT, (WPARAM)cs->hwnd, 0);
			} else if (map_mode_active(g_options.listbox_mode) && is_font_listbox(cs->hwnd)) {
				// ▼を押したクリック (直前に処理された左ボタンのメッセージ) を覚え、一覧は描かれる前に画面の外へ動かす
				g_opener = g_last_click;
				SetWindowPos(cs->hwnd, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				PostMessageW(g_hwnd, WM_APP_LISTBOX, (WPARAM)cs->hwnd, 0);
			}
		}
	}
	return CallNextHookEx(nullptr, code, wparam, lparam);
}

void install_hooks() {
	HMODULE exe = GetModuleHandleW(nullptr);
	g_orig_track_popup_menu = (TrackPopupMenuProc)patch_import(exe, "USER32.dll", "TrackPopupMenu", (void*)hook_track_popup_menu);
	if (!g_orig_track_popup_menu && logger) logger->warn(logger, L"FontMap: TrackPopupMenuを差し替えられませんでした (フォントメニューは地図になりません)");
	g_cwp_hook = SetWindowsHookExW(WH_CALLWNDPROCRET, cwp_hook, nullptr, GetCurrentThreadId());
	g_getmsg_hook = SetWindowsHookExW(WH_GETMESSAGE, getmsg_hook, nullptr, GetCurrentThreadId());
}

void uninstall_hooks() {
	if (g_orig_track_popup_menu) patch_import(GetModuleHandleW(nullptr), "USER32.dll", "TrackPopupMenu", (void*)g_orig_track_popup_menu);
	g_orig_track_popup_menu = nullptr;
	if (g_cwp_hook) UnhookWindowsHookEx(g_cwp_hook);
	g_cwp_hook = nullptr;
	if (g_getmsg_hook) UnhookWindowsHookEx(g_getmsg_hook);
	g_getmsg_hook = nullptr;
}

LRESULT CALLBACK message_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	switch (message) {
	case WM_APP_READY:
		SetTimer(hwnd, TIMER_READY, 100, nullptr);
		return 0;
	case WM_TIMER:
		if (wparam == TIMER_READY) {
			// 本体の起動が終わるまで本体の関数を呼ばない・フックを掛けない
			HWND main = edit_handle->get_host_app_window();
			if (main && IsWindowVisible(main) && IsWindowEnabled(main)) {
				if (!g_ready_since) g_ready_since = GetTickCount64();
				if (GetTickCount64() - g_ready_since >= READY_SETTLE_MS) {
					KillTimer(hwnd, TIMER_READY);
					install_hooks();
					g_ready = true;
				}
			} else {
				g_ready_since = 0;
			}
		} else if (wparam == TIMER_REOPEN) {
			KillTimer(hwnd, TIMER_REOPEN);
			if (!g_reopen_choice.empty() && logger) logger->warn(logger, (L"FontMap: 一覧を開き直せず、フォントを設定できませんでした (" + g_reopen_choice + L")").c_str());
			g_reopen_choice.clear();
		}
		return 0;
	case WM_APP_LISTBOX:
		open_map_for_listbox((HWND)wparam);
		return 0;
	case WM_APP_WRITE_FONT: {
		std::unique_ptr<std::pair<std::wstring, std::wstring>> fonts((std::pair<std::wstring, std::wstring>*)lparam);
		if (!write_font(fonts->first, fonts->second)) {
			if (logger) logger->warn(logger, (L"FontMap: フォントを設定できませんでした (" + fonts->second + L")").c_str());
			MessageBoxW(edit_handle->get_host_app_window(), L"フォントを設定できませんでした。\n(選択中のオブジェクトに、今のフォントの項目が1つだけではありません)", L"FontMap", MB_ICONWARNING);
		}
		return 0;
	}
	case WM_APP_LISTBOX_SELECT:
		select_in_listbox((HWND)wparam);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

//=======================================================================
//	登録
//=======================================================================

void on_project_load(PROJECT_FILE*) {
	static bool loaded = false;
	if (loaded) return;
	loaded = true;
	PostMessageW(g_hwnd, WM_APP_READY, 0, 0);
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	g_ready = false;
	uninstall_hooks();
	if (g_hwnd) DestroyWindow(g_hwnd);
	g_hwnd = nullptr;
	g_fonts.clear();
	g_font_index.clear();
	g_dwrite.Reset();
	g_d2d.Reset();
	if (g_ui_font) DeleteObject(g_ui_font);
	g_ui_font = nullptr;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	edit_handle = host->create_edit_handle();
	load_options();
	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc = message_proc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"FontMapMessageWindow";
	RegisterClassExW(&wc);
	g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
	host->register_config_menu(L"FontMapの設定", on_config);
	host->register_object_item_menu_param(L"フォントマップから選ぶ", false, nullptr, on_item_menu);
	host->register_project_load_handler(on_project_load);
}
