//----------------------------------------------------------------------------------
//	FontMap: UMAPで特徴のベクトルを2次元に並べる
//
//	umap-learn・umap-js と同じ手順(近傍のグラフ → 確率的勾配降下法)を、点の数が数千までの用途向けに簡単に書いたもの。
//	近傍は全ての組の距離を測って求める(距離の関数を自由に決められるように)。初期の配置は主成分分析で決める。
//----------------------------------------------------------------------------------
#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <vector>

struct UmapParams {
	int neighbors = 15;
	double min_dist = 0.08;
	double repulsion = 0.3;
	int epochs = 500;
};

// distance(i, j) : 点iと点jの距離。progress(済んだ量, 全体の量) : 途中経過 (別のスレッドから呼ばれる)
// cancelがtrueになったら途中でやめて空を返す。戻り値は[-1, 1]に収めた座標
std::vector<std::array<double, 2>> run_umap(int count, const std::vector<std::vector<double>>& vectors,
	const std::function<double(int, int)>& distance, const UmapParams& params,
	const std::atomic<bool>& cancel, const std::function<void(int, int)>& progress);
