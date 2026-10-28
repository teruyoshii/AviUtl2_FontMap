//----------------------------------------------------------------------------------
//	FontMap: グリフの形の特徴を取り出す処理と、特徴から距離を測る処理
//
//	特徴の取り方は Font Map (https://ideoaves.github.io/Font_Map/) の方法に沿っている。
//	1文字ごとに、輪郭(アウトライン)から線の太さ・角・曲線の割合・線の向き等を測り、
//	16x16に縮めた画像が全フォントの平均からどれだけ離れているか(typical)を足す。
//----------------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include <dwrite.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// 1文字から取る特徴の数 (typicalを除く。stem3・weight1・contrast2・corner5・curve1・complexity3・direction8・proportion3)
constexpr int RAW_FEATURES = 26;
// typicalを足した、1文字あたりの特徴の数
constexpr int CHAR_FEATURES = RAW_FEATURES + 1;
constexpr int RASTER_SIZE = 16;

// 1文字の解析の結果
struct CharResult {
	bool has = false;										// フォントにその文字があるか
	std::array<double, RAW_FEATURES> raw{};
	std::array<uint8_t, RASTER_SIZE * RASTER_SIZE> raster{};	// 0〜15の濃さ
};

// 特徴を取る文字の1組の解析の結果
struct SetResult {
	bool missing = true;			// 組の文字が1つも無い
	std::vector<CharResult> chars;
};

// 文字列をコードポイントの並びにする (サロゲートペアを1文字にまとめる)
std::vector<UINT32> to_codepoints(const std::wstring& text);

// フォントの1組の文字を解析する
SetResult analyze_set(IDWriteFontFace* face, const std::vector<UINT32>& chars);

// 解析の結果から、全フォントで比べられる特徴の並び(ベクトル)を作る
//  sets[i][s] : i番目のフォントの、s番目の組の結果 (nullptrなら解析していない=無いものとして扱う)
//  char_counts : 組ごとの文字数
// 戻り値の各ベクトルの後ろには、文字ごとに「その文字があるか」(1/0)が付く
struct FeatureSpace {
	std::vector<std::vector<double>> vectors;
	std::vector<std::pair<int, int>> unit_ranges;	// 文字ごとの、ベクトルの中の範囲
	double distance(const std::vector<double>& a, const std::vector<double>& b) const;
};
FeatureSpace build_feature_space(const std::vector<std::vector<const SetResult*>>& sets, const std::vector<int>& char_counts);
