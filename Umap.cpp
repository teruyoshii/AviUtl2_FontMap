//----------------------------------------------------------------------------------
//	FontMap: UMAPで特徴のベクトルを2次元に並べる
//----------------------------------------------------------------------------------
#include "Umap.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

namespace {

// 低次元での近さの曲線 1/(1+a*d^(2b)) を、min_distで平らになる曲線に合わせる (umapのfind_ab_params)
void fit_ab(double min_dist, double spread, double& a, double& b) {
	const int N = 300;
	std::vector<double> xs(N), ys(N);
	for (int i = 0; i < N; i++) {
		xs[i] = spread * 3 * i / (N - 1);
		ys[i] = xs[i] < min_dist ? 1 : std::exp(-(xs[i] - min_dist) / spread);
	}
	auto error = [&](double pa, double pb) {
		double e = 0;
		for (int i = 0; i < N; i++) {
			double y = 1 / (1 + pa * std::pow(xs[i], 2 * pb));
			e += (y - ys[i]) * (y - ys[i]);
		}
		return e;
	};
	// Levenberg-Marquardt法 (微分は数値で求める)
	a = 1; b = 1;
	double lambda = 1e-3, current = error(a, b);
	for (int iter = 0; iter < 200; iter++) {
		double jtj[2][2] = {}, jtr[2] = {};
		for (int i = 0; i < N; i++) {
			double x = xs[i], p = std::pow(x, 2 * b), y = 1 / (1 + a * p), r = ys[i] - y;
			double da = -p * y * y;
			double db = x > 0 ? -a * p * 2 * std::log(x) * y * y : 0;
			jtj[0][0] += da * da; jtj[0][1] += da * db; jtj[1][1] += db * db;
			jtr[0] += da * r; jtr[1] += db * r;
		}
		jtj[1][0] = jtj[0][1];
		bool improved = false;
		for (int t = 0; t < 20 && !improved; t++) {
			double m00 = jtj[0][0] * (1 + lambda), m11 = jtj[1][1] * (1 + lambda), m01 = jtj[0][1];
			double det = m00 * m11 - m01 * m01;
			if (!det) break;
			double sa = (jtr[0] * m11 - jtr[1] * m01) / det, sb = (m00 * jtr[1] - m01 * jtr[0]) / det;
			double na = a + sa, nb = b + sb, e = error(na, nb);
			if (na > 0 && nb > 0 && e < current) {
				a = na; b = nb; current = e; lambda /= 10; improved = true;
			} else {
				lambda *= 10;
			}
		}
		if (!improved) break;
	}
}

// 主成分分析の第1・第2主成分の方向へ写した座標 (べき乗法で求める)
std::vector<std::array<double, 2>> pca_init(const std::vector<std::vector<double>>& vectors, std::mt19937& rng) {
	size_t n = vectors.size(), dim = vectors.empty() ? 0 : vectors[0].size();
	std::vector<double> mean(dim, 0);
	for (auto& v : vectors) for (size_t k = 0; k < dim; k++) mean[k] += v[k] / n;
	std::vector<std::vector<double>> axes;
	std::normal_distribution<double> normal(0, 1);
	for (int axis = 0; axis < 2; axis++) {
		std::vector<double> dir(dim);
		for (auto& d : dir) d = normal(rng);
		for (int iter = 0; iter < 100; iter++) {
			std::vector<double> next(dim, 0);
			for (auto& v : vectors) {
				double dot = 0;
				for (size_t k = 0; k < dim; k++) dot += (v[k] - mean[k]) * dir[k];
				for (size_t k = 0; k < dim; k++) next[k] += (v[k] - mean[k]) * dot;
			}
			for (auto& prev : axes) {
				double dot = 0;
				for (size_t k = 0; k < dim; k++) dot += next[k] * prev[k];
				for (size_t k = 0; k < dim; k++) next[k] -= dot * prev[k];
			}
			double norm = 0;
			for (double x : next) norm += x * x;
			norm = std::sqrt(norm);
			if (!norm) break;
			for (size_t k = 0; k < dim; k++) dir[k] = next[k] / norm;
		}
		axes.push_back(dir);
	}
	std::vector<std::array<double, 2>> out(n);
	double max_abs = 0;
	for (size_t i = 0; i < n; i++) {
		for (int axis = 0; axis < 2; axis++) {
			double dot = 0;
			for (size_t k = 0; k < dim; k++) dot += (vectors[i][k] - mean[k]) * axes[axis][k];
			out[i][axis] = dot;
			max_abs = std::max(max_abs, std::abs(dot));
		}
	}
	// umap-learnの初期化と同じく、最大が10になるように広げ、少しだけ揺らす
	std::normal_distribution<double> noise(0, 1e-4);
	for (auto& p : out) for (auto& x : p) x = (max_abs ? x * 10 / max_abs : 0) + noise(rng);
	return out;
}

double clip(double v) {
	return std::max(-4.0, std::min(4.0, v));
}

}	// namespace

std::vector<std::array<double, 2>> run_umap(int count, const std::vector<std::vector<double>>& vectors,
	const std::function<double(int, int)>& distance, const UmapParams& params,
	const std::atomic<bool>& cancel, const std::function<void(int, int)>& progress) {
	if (count < 3) return {};
	int k = std::min(params.neighbors, count - 1) + 1;	// 自分自身を含める (umapと同じ)
	int work_total = count + params.epochs;

	// 近傍を、全ての組の距離から求める
	std::vector<std::vector<std::pair<double, int>>> knn(count);
	std::vector<std::pair<double, int>> row(count);
	for (int i = 0; i < count; i++) {
		if (cancel) return {};
		for (int j = 0; j < count; j++) row[j] = { i == j ? 0.0 : distance(i, j), j };
		std::partial_sort(row.begin(), row.begin() + k, row.end(), [&](auto& p, auto& q) {
			if (p.second == i) return q.second != i;
			if (q.second == i) return false;
			return p.first < q.first;
		});
		knn[i].assign(row.begin(), row.begin() + k);
		if (i % 32 == 0) progress(i, work_total);
	}

	// 近傍までの距離から、点ごとの広がり(sigma)と、最も近い点までの距離(rho)を決める (smooth_knn_dist)
	double target = std::log2((double)k);
	double mean_all = 0;
	for (auto& r : knn) for (auto& p : r) mean_all += p.first / ((double)count * k);
	std::unordered_map<unsigned long long, double> weights;
	auto key = [&](int i, int j) { return (unsigned long long)i * count + j; };
	for (int i = 0; i < count; i++) {
		double rho = 0;
		for (auto& p : knn[i]) if (p.first > 0) { rho = p.first; break; }
		double lo = 0, hi = INFINITY, mid = 1;
		for (int iter = 0; iter < 64; iter++) {
			double sum = 0;
			for (int j = 1; j < k; j++) {
				double d = knn[i][j].first - rho;
				sum += d > 0 ? std::exp(-d / mid) : 1;
			}
			if (std::abs(sum - target) < 1e-5) break;
			if (sum > target) { hi = mid; mid = (lo + hi) / 2; }
			else { lo = mid; mid = hi == INFINITY ? mid * 2 : (lo + hi) / 2; }
		}
		double mean_i = 0;
		for (auto& p : knn[i]) mean_i += p.first / k;
		double floor = 1e-3 * (rho > 0 ? mean_i : mean_all);
		double sigma = std::max(mid, floor);
		for (auto& p : knn[i]) {
			if (p.second == i) continue;
			double d = p.first - rho;
			weights[key(i, p.second)] = d <= 0 || !sigma ? 1 : std::exp(-d / sigma);
		}
	}
	// 向きの無いグラフにする (w + w^T - w*w^T)
	struct GraphEdge { int head, tail; double w; };
	std::vector<GraphEdge> edges;
	for (auto& [ij, w] : weights) {
		int i = (int)(ij / count), j = (int)(ij % count);
		auto t = weights.find(key(j, i));
		double wt = t != weights.end() ? t->second : 0;
		edges.push_back({ i, j, w + wt - w * wt });
		if (t == weights.end()) edges.push_back({ j, i, w });
	}
	std::sort(edges.begin(), edges.end(), [](auto& p, auto& q) { return p.head != q.head ? p.head < q.head : p.tail < q.tail; });
	double max_w = 0;
	for (auto& e : edges) max_w = std::max(max_w, e.w);
	edges.erase(std::remove_if(edges.begin(), edges.end(), [&](auto& e) { return e.w < max_w / params.epochs; }), edges.end());

	// 配置を、近傍に引き寄せ・ランダムな点から遠ざける操作の繰り返しで決める
	double a, b;
	fit_ab(params.min_dist, 1.0, a, b);
	std::mt19937 rng(42);
	auto emb = pca_init(vectors, rng);
	std::uniform_int_distribution<int> pick(0, count - 1);
	const double NEGATIVE_RATE = 5;
	size_t m = edges.size();
	std::vector<double> per_sample(m), next_sample(m), per_negative(m), next_negative(m);
	for (size_t e = 0; e < m; e++) {
		per_sample[e] = max_w / edges[e].w;
		next_sample[e] = per_sample[e];
		per_negative[e] = per_sample[e] / NEGATIVE_RATE;
		next_negative[e] = per_negative[e];
	}
	for (int n = 0; n < params.epochs; n++) {
		if (cancel) return {};
		double alpha = 1 - (double)n / params.epochs;
		for (size_t e = 0; e < m; e++) {
			if (next_sample[e] > n) continue;
			auto& current = emb[edges[e].head];
			auto& other = emb[edges[e].tail];
			double d2 = std::pow(current[0] - other[0], 2) + std::pow(current[1] - other[1], 2);
			double coeff = d2 > 0 ? -2 * a * b * std::pow(d2, b - 1) / (a * std::pow(d2, b) + 1) : 0;
			for (int d = 0; d < 2; d++) {
				double grad = clip(coeff * (current[d] - other[d]));
				current[d] += grad * alpha;
				other[d] -= grad * alpha;
			}
			next_sample[e] += per_sample[e];
			int negatives = (int)((n - next_negative[e]) / per_negative[e]);
			for (int p = 0; p < negatives; p++) {
				int t = pick(rng);
				if (t == edges[e].head) continue;
				auto& far = emb[t];
				double fd2 = std::pow(current[0] - far[0], 2) + std::pow(current[1] - far[1], 2);
				double rc = fd2 > 0 ? 2 * params.repulsion * b / ((0.001 + fd2) * (a * std::pow(fd2, b) + 1)) : 0;
				for (int d = 0; d < 2; d++) current[d] += (rc > 0 ? clip(rc * (current[d] - far[d])) : 4) * alpha;
			}
			next_negative[e] += negatives * per_negative[e];
		}
		if (n % 10 == 0) progress(count + n, work_total);
	}

	// [-1, 1]に収める
	double min_x = INFINITY, max_x = -INFINITY, min_y = INFINITY, max_y = -INFINITY;
	for (auto& p : emb) {
		min_x = std::min(min_x, p[0]); max_x = std::max(max_x, p[0]);
		min_y = std::min(min_y, p[1]); max_y = std::max(max_y, p[1]);
	}
	double half = std::max(max_x - min_x, max_y - min_y) / 2;
	if (!half) half = 1;
	for (auto& p : emb) {
		p[0] = (p[0] - (min_x + max_x) / 2) / half;
		p[1] = (p[1] - (min_y + max_y) / 2) / half;
	}
	return emb;
}
