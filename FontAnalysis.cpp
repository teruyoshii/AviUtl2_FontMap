//----------------------------------------------------------------------------------
//	FontMap: グリフの形の特徴を取り出す処理と、特徴から距離を測る処理
//----------------------------------------------------------------------------------
#include "FontAnalysis.h"
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>

namespace {

const double PI = 3.14159265358979323846;
const double CORNER_DEG = 25;			// これより大きく曲がる所を角とみなす
const double SHORT_SEGMENT = 0.02;		// これより短い線分を細かい線分として数える (em単位)
const int THICKNESS_SAMPLES = 120;		// 線の太さを測る点の数
const int DIR_BINS = 8;					// 線の向きを分ける数
const int FLATTEN_STEPS = 8;			// 曲線を折れ線にする時の分割数
const double NaN = std::numeric_limits<double>::quiet_NaN();

// 特徴の組ごとの重み。Font Mapと同じ値
struct Group { int size; double weight; };
const Group GROUPS[] = {
	{ 1, 2.5 },	// typical
	{ 3, 2.5 },	// stem
	{ 1, 2.0 },	// weight
	{ 2, 2.0 },	// contrast
	{ 5, 1.5 },	// corner
	{ 1, 1.0 },	// curve
	{ 3, 1.0 },	// complexity
	{ DIR_BINS, 1.0 },	// direction
	{ 3, 0.7 },	// proportion
};

struct Pt { double x, y; };

// 輪郭の1区間。直線は点2つ、曲線(3次ベジェ)は点4つ
struct Segment {
	bool curve;
	std::vector<Pt> ctrl;
};
using Contour = std::vector<Segment>;

// DirectWriteの輪郭を受け取る
class OutlineSink : public IDWriteGeometrySink {
public:
	std::vector<Contour> contours;
	ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
	ULONG STDMETHODCALLTYPE Release() override { return --ref; }
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
		if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1SimplifiedGeometrySink)) {
			*out = this;
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override {}
	void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}
	void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) override {
		contours.emplace_back();
		start = prev = { p.x, p.y };
	}
	void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, UINT32 count) override {
		for (UINT32 i = 0; i < count; i++) {
			Pt p = { points[i].x, points[i].y };
			if (p.x == prev.x && p.y == prev.y) continue;
			contours.back().push_back({ false, { prev, p } });
			prev = p;
		}
	}
	void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* beziers, UINT32 count) override {
		for (UINT32 i = 0; i < count; i++) {
			const auto& b = beziers[i];
			Pt p = { b.point3.x, b.point3.y };
			contours.back().push_back({ true, { prev, { b.point1.x, b.point1.y }, { b.point2.x, b.point2.y }, p } });
			prev = p;
		}
	}
	void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) override {
		if (prev.x != start.x || prev.y != start.y) contours.back().push_back({ false, { prev, start } });
		prev = start;
		if (contours.back().empty()) contours.pop_back();
	}
	HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
private:
	ULONG ref = 1;
	Pt start{}, prev{};
};

Pt bezier(const std::vector<Pt>& pts, double t) {
	std::vector<Pt> p = pts;
	for (size_t n = p.size(); n > 1; n--) {
		for (size_t i = 0; i + 1 < n; i++) p[i] = { p[i].x + (p[i + 1].x - p[i].x) * t, p[i].y + (p[i + 1].y - p[i].y) * t };
	}
	return p[0];
}

// 区間の始め(atEnd=falseの時)・終わりの向き
Pt tangent(const std::vector<Pt>& ctrl, bool at_end) {
	std::vector<Pt> pts = ctrl;
	if (at_end) std::reverse(pts.begin(), pts.end());
	Pt q = pts[0];
	for (auto& p : pts) {
		if (p.x != pts[0].x || p.y != pts[0].y) { q = p; break; }
	}
	return at_end ? Pt{ pts[0].x - q.x, pts[0].y - q.y } : Pt{ q.x - pts[0].x, q.y - pts[0].y };
}

struct Edge { double x0, y0, x1, y1; };

// 線の太さ: 輪郭上の点から内側へ光線を飛ばし、反対側の輪郭までの距離を測る
std::array<double, 3> thickness(const std::vector<Edge>& edges, double perimeter) {
	const double eps = 1e-4;
	auto inside = [&](double px, double py) {
		int w = 0;
		for (auto& e : edges) {
			double cross = (e.x1 - e.x0) * (py - e.y0) - (px - e.x0) * (e.y1 - e.y0);
			if (e.y0 <= py) { if (e.y1 > py && cross > 0) w++; }
			else { if (e.y1 <= py && cross < 0) w--; }
		}
		return w != 0;
	};
	auto ray = [&](double ox, double oy, double nx, double ny) {
		double best = INFINITY;
		for (auto& e : edges) {
			double ex = e.x1 - e.x0, ey = e.y1 - e.y0, den = nx * ey - ny * ex;
			if (!den) continue;
			double t = ((e.x0 - ox) * ey - (e.y0 - oy) * ex) / den, s = ((e.x0 - ox) * ny - (e.y0 - oy) * nx) / den;
			if (t > eps && s >= 0 && s <= 1 && t < best) best = t;
		}
		return best;
	};
	double step = perimeter / THICKNESS_SAMPLES, carry = step / 2;
	std::vector<double> widths;
	for (auto& e : edges) {
		double l = std::hypot(e.x1 - e.x0, e.y1 - e.y0), d = carry;
		for (; d < l; d += step) {
			double px = e.x0 + (e.x1 - e.x0) * d / l, py = e.y0 + (e.y1 - e.y0) * d / l;
			double nx = -(e.y1 - e.y0) / l, ny = (e.x1 - e.x0) / l;
			if (!inside(px + nx * eps, py + ny * eps)) { nx = -nx; ny = -ny; }
			double t = ray(px, py, nx, ny);
			if (t < INFINITY) widths.push_back(t);
		}
		carry = d - l;
	}
	std::sort(widths.begin(), widths.end());
	auto pct = [&](double r) {
		if (widths.empty()) return 1e-3;
		double v = widths[std::min(widths.size() - 1, (size_t)(widths.size() * r))];
		return v ? v : 1e-3;
	};
	return { pct(0.5), std::log(pct(0.8) / pct(0.2)), pct(0.9) / pct(0.5) };
}

std::array<double, RAW_FEATURES> measure_glyph(const std::vector<Contour>& contours, double stem_h, double stem_v, std::vector<Edge>& edges) {
	std::array<double, DIR_BINS> dir{};
	std::array<double, 3> corner_hist{};
	double perimeter = 0, curve_len = 0, corner_sum = 0;
	int short_count = 0, corners = 0, seg_count = 0;
	double min_x = INFINITY, max_x = -INFINITY, min_y = INFINITY, max_y = -INFINITY;
	for (auto& contour : contours) {
		for (size_t k = 0; k < contour.size(); k++) {
			const Segment& s = contour[k];
			seg_count++;
			std::vector<Pt> pts;
			if (s.curve) {
				for (int j = 0; j <= FLATTEN_STEPS; j++) pts.push_back(bezier(s.ctrl, (double)j / FLATTEN_STEPS));
			} else {
				pts = s.ctrl;
			}
			double len = 0;
			for (size_t j = 1; j < pts.size(); j++) {
				Pt a = pts[j - 1], b = pts[j];
				double l = std::hypot(b.x - a.x, b.y - a.y);
				min_x = std::min(min_x, b.x); max_x = std::max(max_x, b.x);
				min_y = std::min(min_y, b.y); max_y = std::max(max_y, b.y);
				if (!l) continue;
				len += l;
				edges.push_back({ a.x, a.y, b.x, b.y });
				double angle = std::fmod(std::atan2(b.y - a.y, b.x - a.x) + PI, PI);
				dir[std::min(DIR_BINS - 1, (int)(angle / PI * DIR_BINS))] += l;
			}
			perimeter += len;
			if (s.curve) curve_len += len;
			if (len < SHORT_SEGMENT) short_count++;
			Pt a = tangent(s.ctrl, true), b = tangent(contour[(k + 1) % contour.size()].ctrl, false);
			double norm = std::hypot(a.x, a.y) * std::hypot(b.x, b.y);
			double cos = (a.x * b.x + a.y * b.y) / (norm ? norm : 1);
			double turn = std::acos(std::max(-1.0, std::min(1.0, cos))) * 180 / PI;
			if (turn <= CORNER_DEG) continue;
			corners++;
			corner_sum += turn;
			corner_hist[std::min(2, (int)((turn - CORNER_DEG) / (180 - CORNER_DEG) * 4))]++;
		}
	}
	auto th = thickness(edges, perimeter);
	double c = corners ? corners : 1;
	std::array<double, RAW_FEATURES> f{};
	int i = 0;
	f[i++] = stem_h; f[i++] = stem_v; f[i++] = std::log(stem_v / stem_h);
	f[i++] = th[0];
	f[i++] = th[1]; f[i++] = th[2];
	f[i++] = corners; f[i++] = corners ? corner_sum / corners : 0;
	for (double h : corner_hist) f[i++] = h / c;
	f[i++] = curve_len / perimeter;
	f[i++] = (double)contours.size(); f[i++] = seg_count; f[i++] = (double)short_count / seg_count;
	for (double d : dir) f[i++] = d / perimeter;
	f[i++] = (max_x - min_x) / (max_y - min_y); f[i++] = -min_y; f[i++] = max_y;
	return f;
}

// 輪郭を16x16に縮めて塗る。縦横それぞれ4倍の細かさで塗って平均する
void rasterize(const std::vector<Contour>& contours, const std::vector<Edge>& edges, std::array<uint8_t, RASTER_SIZE * RASTER_SIZE>& out) {
	const int SUB = 4, N = RASTER_SIZE * SUB;
	double min_x = INFINITY, max_x = -INFINITY, min_y = INFINITY, max_y = -INFINITY;
	for (auto& c : contours) for (auto& s : c) for (auto& p : s.ctrl) {
		min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x);
		min_y = std::min(min_y, p.y); max_y = std::max(max_y, p.y);
	}
	double scale = (RASTER_SIZE - 2) / std::max({ max_x - min_x, max_y - min_y, 1e-6 });
	double ox = RASTER_SIZE / 2.0 - (min_x + max_x) / 2 * scale, oy = RASTER_SIZE / 2.0 - (min_y + max_y) / 2 * scale;
	std::vector<int> cover(RASTER_SIZE * RASTER_SIZE, 0);
	std::vector<std::pair<double, int>> crossings;
	for (int row = 0; row < N; row++) {
		double y = (row + 0.5) / SUB;
		crossings.clear();
		for (auto& e : edges) {
			double y0 = e.y0 * scale + oy, y1 = e.y1 * scale + oy;
			if ((y0 <= y) == (y1 <= y)) continue;
			double x0 = e.x0 * scale + ox, x1 = e.x1 * scale + ox;
			crossings.push_back({ x0 + (y - y0) / (y1 - y0) * (x1 - x0), y1 > y0 ? 1 : -1 });
		}
		std::sort(crossings.begin(), crossings.end());
		int winding = 0;
		for (size_t i = 0; i + 1 < crossings.size(); i++) {
			winding += crossings[i].second;
			if (!winding) continue;
			int from = std::max(0, (int)std::ceil(crossings[i].first * SUB - 0.5));
			int to = std::min(N - 1, (int)std::floor(crossings[i + 1].first * SUB - 0.5));
			for (int col = from; col <= to; col++) cover[(row / SUB) * RASTER_SIZE + col / SUB]++;
		}
	}
	for (int i = 0; i < RASTER_SIZE * RASTER_SIZE; i++) out[i] = (uint8_t)std::lround(cover[i] * 15.0 / (SUB * SUB));
}

//-----------------------------------------------------------------------
//	CFFのヒント値 (StdHW・StdVW)
//-----------------------------------------------------------------------

using Dict = std::map<int, std::vector<double>>;

Dict read_dict(const uint8_t* bytes, size_t i, size_t end) {
	Dict dict;
	std::vector<double> ops;
	while (i < end) {
		int b = bytes[i++];
		if (b <= 21) {
			int op = b;
			if (b == 12 && i < end) op = 1200 + bytes[i++];
			dict[op] = ops;
			ops.clear();
		} else if (b == 28 && i + 2 <= end) {
			ops.push_back((int16_t)(bytes[i] << 8 | bytes[i + 1]));
			i += 2;
		} else if (b == 29 && i + 4 <= end) {
			ops.push_back((int32_t)((uint32_t)bytes[i] << 24 | bytes[i + 1] << 16 | bytes[i + 2] << 8 | bytes[i + 3]));
			i += 4;
		} else if (b == 30) {
			std::string s;
			bool done = false;
			while (!done && i < end) {
				int v = bytes[i++];
				for (int n : { v >> 4, v & 15 }) {
					if (n == 15) { done = true; break; }
					static const char* table[] = { ".", "E", "E-", "", "-" };
					if (n < 10) s += (char)('0' + n); else s += table[n - 10];
				}
			}
			ops.push_back(std::atof(s.c_str()));
		} else if (b <= 246) {
			ops.push_back(b - 139);
		} else if (b <= 250 && i < end) {
			ops.push_back((b - 247) * 256 + bytes[i++] + 108);
		} else if (b <= 254 && i < end) {
			ops.push_back(-(b - 251) * 256 - bytes[i++] - 108);
		}
	}
	return dict;
}

// CFFのINDEXの各データの範囲。posはINDEXの直後に進める
bool read_index(const uint8_t* p, size_t size, size_t& pos, std::vector<std::pair<size_t, size_t>>& items) {
	if (pos + 2 > size) return false;
	int count = p[pos] << 8 | p[pos + 1];
	if (count == 0) { pos += 2; return true; }
	if (pos + 3 > size) return false;
	int off_size = p[pos + 2];
	size_t offsets = pos + 3, data = offsets + (count + 1) * off_size - 1;
	if (off_size < 1 || off_size > 4 || offsets + (count + 1) * off_size > size) return false;
	auto offset = [&](int k) {
		size_t v = 0;
		for (int b = 0; b < off_size; b++) v = v << 8 | p[offsets + k * off_size + b];
		return v;
	};
	for (int k = 0; k < count; k++) {
		size_t a = data + offset(k), b = data + offset(k + 1);
		if (b > size || a > b) return false;
		items.push_back({ a, b });
	}
	pos = data + offset(count);
	return true;
}

// フォントの全グリフについて、グリフが使うPrivate DICTの番号と、その番号ごとのStdHW・StdVWを取る
struct Stems {
	std::vector<std::pair<double, double>> by_fd;
	std::vector<uint8_t> fd_select;	// 空ならFD 0
	std::pair<double, double> get(UINT16 glyph) const {
		size_t fd = glyph < fd_select.size() ? fd_select[glyph] : 0;
		return fd < by_fd.size() ? by_fd[fd] : std::make_pair(NaN, NaN);
	}
};

Stems read_stems(IDWriteFontFace* face, double upem, UINT32 glyph_count) {
	Stems stems;
	const void* data = nullptr;
	UINT32 size = 0;
	void* context = nullptr;
	BOOL exists = FALSE;
	if (FAILED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('C', 'F', 'F', ' '), &data, &size, &context, &exists)) || !exists) return stems;
	const uint8_t* p = (const uint8_t*)data;
	auto stems_of = [&](const Dict& font_dict) {
		auto it = font_dict.find(18);
		if (it == font_dict.end() || it->second.size() < 2) return std::make_pair(NaN, NaN);
		size_t priv_size = (size_t)it->second[0], priv_offset = (size_t)it->second[1];
		if (priv_offset + priv_size > size) return std::make_pair(NaN, NaN);
		Dict priv = read_dict(p, priv_offset, priv_offset + priv_size);
		auto first = [&](int op) { auto f = priv.find(op); return f != priv.end() && !f->second.empty() ? f->second[0] / upem : NaN; };
		return std::make_pair(first(10), first(11));
	};
	do {
		if (size < 4) break;
		size_t pos = p[2];
		std::vector<std::pair<size_t, size_t>> names, tops;
		if (!read_index(p, size, pos, names) || !read_index(p, size, pos, tops) || tops.empty()) break;
		Dict top = read_dict(p, tops[0].first, tops[0].second);
		auto fd_array = top.find(1236), fd_select = top.find(1237);
		if (fd_array == top.end() || fd_array->second.empty()) {
			stems.by_fd.push_back(stems_of(top));
			break;
		}
		size_t fa = (size_t)fd_array->second[0];
		std::vector<std::pair<size_t, size_t>> fds;
		if (!read_index(p, size, fa, fds)) break;
		for (auto& fd : fds) stems.by_fd.push_back(stems_of(read_dict(p, fd.first, fd.second)));
		if (fd_select == top.end() || fd_select->second.empty()) break;
		size_t fs = (size_t)fd_select->second[0];
		if (fs >= size) break;
		int format = p[fs];
		if (format == 0 && fs + 1 + glyph_count <= size) {
			stems.fd_select.assign(p + fs + 1, p + fs + 1 + glyph_count);
		} else if (format == 3 && fs + 3 <= size) {
			int ranges = p[fs + 1] << 8 | p[fs + 2];
			if (fs + 3 + ranges * 3 + 2 > size) break;
			stems.fd_select.assign(glyph_count, 0);
			for (int r = 0; r < ranges; r++) {
				size_t q = fs + 3 + r * 3;
				UINT32 first = p[q] << 8 | p[q + 1], next = p[q + 3] << 8 | p[q + 4];
				for (UINT32 g = first; g < next && g < glyph_count; g++) stems.fd_select[g] = p[q + 2];
			}
		}
	} while (false);
	face->ReleaseFontTable(context);
	return stems;
}

}	// namespace

std::vector<UINT32> to_codepoints(const std::wstring& text) {
	std::vector<UINT32> out;
	for (size_t i = 0; i < text.size(); i++) {
		UINT32 c = text[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
			c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
			i++;
		}
		out.push_back(c);
	}
	return out;
}

SetResult analyze_set(IDWriteFontFace* face, const std::vector<UINT32>& chars) {
	SetResult result;
	result.chars.resize(chars.size());
	std::vector<UINT16> glyphs(chars.size());
	if (chars.empty() || FAILED(face->GetGlyphIndices(chars.data(), (UINT32)chars.size(), glyphs.data()))) return result;
	DWRITE_FONT_METRICS metrics;
	face->GetMetrics(&metrics);
	Stems stems = read_stems(face, metrics.designUnitsPerEm, face->GetGlyphCount());
	for (size_t c = 0; c < chars.size(); c++) {
		if (!glyphs[c]) continue;
		OutlineSink sink;
		if (FAILED(face->GetGlyphRunOutline(1.0f, &glyphs[c], nullptr, nullptr, 1, FALSE, FALSE, &sink)) || sink.contours.empty()) continue;
		auto stem = stems.get(glyphs[c]);
		std::vector<Edge> edges;
		CharResult& r = result.chars[c];
		r.raw = measure_glyph(sink.contours, stem.first, stem.second, edges);
		rasterize(sink.contours, edges, r.raster);
		r.has = true;
		result.missing = false;
	}
	return result;
}

FeatureSpace build_feature_space(const std::vector<std::vector<const SetResult*>>& sets, const std::vector<int>& char_counts) {
	FeatureSpace space;
	size_t fonts = sets.size();
	auto present = [&](size_t i, size_t s, int c) {
		const SetResult* r = sets[i][s];
		return r && !r->missing && c < (int)r->chars.size() && r->chars[c].has;
	};
	std::vector<std::vector<double>> rows(fonts);
	std::vector<std::vector<bool>> masks(fonts);
	int offset = 0;
	for (size_t s = 0; s < char_counts.size(); s++) {
		int count = char_counts[s], cols = count * CHAR_FEATURES;
		// 文字ごとの、全フォントの平均の画像
		std::vector<std::array<double, RASTER_SIZE * RASTER_SIZE>> mean(count);
		for (int c = 0; c < count; c++) {
			int n = 0;
			mean[c].fill(0);
			for (size_t i = 0; i < fonts; i++) {
				if (!present(i, s, c)) continue;
				for (int k = 0; k < RASTER_SIZE * RASTER_SIZE; k++) mean[c][k] += sets[i][s]->chars[c].raster[k] / 15.0;
				n++;
			}
			if (n) for (auto& v : mean[c]) v /= n;
		}
		// フォントごとの生の値 (無い文字はNaN)
		std::vector<std::vector<double>> block(fonts, std::vector<double>(cols, NaN));
		for (size_t i = 0; i < fonts; i++) {
			for (int c = 0; c < count; c++) {
				if (!present(i, s, c)) continue;
				const CharResult& r = sets[i][s]->chars[c];
				double typical = 0;
				for (int k = 0; k < RASTER_SIZE * RASTER_SIZE; k++) typical += std::pow(r.raster[k] / 15.0 - mean[c][k], 2);
				block[i][c * CHAR_FEATURES] = std::sqrt(typical);
				for (int k = 0; k < RAW_FEATURES; k++) block[i][c * CHAR_FEATURES + 1 + k] = r.raw[k];
			}
		}
		// 列ごとに標準化し、組の重みを掛ける (組の列の数の平方根で割り、列の多い組が強くならないようにする)
		std::vector<int> labels;
		for (int g = 0; g < (int)std::size(GROUPS); g++) for (int k = 0; k < GROUPS[g].size; k++) labels.push_back(g);
		for (int j = 0; j < cols; j++) {
			const Group& group = GROUPS[labels[j % CHAR_FEATURES]];
			double sum = 0, sq = 0;
			int n = 0;
			for (size_t i = 0; i < fonts; i++) if (std::isfinite(block[i][j])) { sum += block[i][j]; n++; }
			double m = sum / (n ? n : 1);
			for (size_t i = 0; i < fonts; i++) if (std::isfinite(block[i][j])) sq += std::pow(block[i][j] - m, 2);
			double sd = std::sqrt(sq / (n ? n : 1));
			if (!sd) sd = 1;
			double factor = group.weight / sd / std::sqrt((double)group.size * count);
			for (size_t i = 0; i < fonts; i++) {
				double& v = block[i][j];
				if (!present(i, s, j / CHAR_FEATURES)) v = 0;
				else v = std::isfinite(v) ? (v - m) * factor : 0;
			}
		}
		for (size_t i = 0; i < fonts; i++) {
			rows[i].insert(rows[i].end(), block[i].begin(), block[i].end());
			for (int c = 0; c < count; c++) masks[i].push_back(present(i, s, c));
		}
		for (int c = 0; c < count; c++) {
			space.unit_ranges.push_back({ offset, offset + CHAR_FEATURES });
			offset += CHAR_FEATURES;
		}
	}
	space.vectors.resize(fonts);
	for (size_t i = 0; i < fonts; i++) {
		space.vectors[i] = rows[i];
		for (bool m : masks[i]) space.vectors[i].push_back(m ? 1 : 0);
	}
	return space;
}

// 両方のフォントにある文字だけで測った距離。共通の文字が無ければ遠いものとする
double FeatureSpace::distance(const std::vector<double>& a, const std::vector<double>& b) const {
	const double FAR_DISTANCE = 100;
	int total = unit_ranges.empty() ? 0 : unit_ranges.back().second;
	double sum = 0;
	int n = 0;
	for (size_t u = 0; u < unit_ranges.size(); u++) {
		if (!a[total + u] || !b[total + u]) continue;
		for (int k = unit_ranges[u].first; k < unit_ranges[u].second; k++) sum += (a[k] - b[k]) * (a[k] - b[k]);
		n += unit_ranges[u].second - unit_ranges[u].first;
	}
	return n ? std::sqrt(sum * total / n) : FAR_DISTANCE;
}
