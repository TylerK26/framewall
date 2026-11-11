// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "layout.h"

#include <algorithm>
#include <cmath>
#include <regex>

bool parse_wall(const std::string &app, Wall &wall, std::string &err) {
    static const std::regex re(R"(^Wall (\d+)x(\d+)x(\d+)@(\d+) (\w+) \+(\d+)x(\d+) \((\d+)x(\d+)\)$)");
    std::smatch m;
    if (!std::regex_match(app, m, re)) {
        err = "\"" + app + "\" is not a framewall wall preset (update framewall on the host?)";
        return false;
    }
    Wall w;
    w.count = std::stoi(m[1]); w.vw = std::stoi(m[2]); w.vh = std::stoi(m[3]); w.hz = std::stoi(m[4]);
    w.shape = m[5];
    w.bw = std::stoi(m[6]); w.bh = std::stoi(m[7]);
    w.width = std::stoi(m[8]); w.height = std::stoi(m[9]);

    // Same rules as plan() in host/framewall.
    const int n = w.count, vw = w.vw, vh = w.vh, bw = w.bw, bh = w.bh;
    std::vector<Rect> virt(n);
    Rect block{0, 0, bw, bh};
    int W = 0, H = 0;
    if (w.shape == "top" || (w.shape == "grid" && n <= 1)) {
        const int row = n * vw;
        W = std::max(row, bw);
        for (int i = 0; i < n; i++) virt[i] = {(W - row) / 2 + i * vw, 0, vw, vh};
        block.x = (W - bw) / 2;
        block.y = n ? vh : 0;
        H = (n ? vh : 0) + bh;
    } else if (w.shape == "row") {
        const int left = n / 2;
        H = std::max(n ? vh : 0, bh);
        int x = 0;
        for (int i = 0; i < left; i++) { virt[i] = {x, (H - vh) / 2, vw, vh}; x += vw; }
        block.x = x; block.y = (H - bh) / 2; x += bw;
        for (int i = left; i < n; i++) { virt[i] = {x, (H - vh) / 2, vw, vh}; x += vw; }
        W = x;
    } else if (w.shape == "grid") {
        double best = -1;
        int cols = 1, gw = 0, gh = 0;
        for (int c = 1; c <= n; c++) {
            const int gw_ = c * vw, gh_ = (n + c - 1) / c * vh;
            const int W_ = std::max(gw_, bw), H_ = gh_ + bh;
            const double score = std::fabs(double(W_) / H_ - 16.0 / 9.0);
            if (best < 0 || score < best) { best = score; cols = c; gw = gw_; gh = gh_; W = W_; H = H_; }
        }
        for (int i = 0; i < n; i++) virt[i] = {(W - gw) / 2 + (i % cols) * vw, (i / cols) * vh, vw, vh};
        block.x = (W - bw) / 2;
        block.y = gh;
    } else {
        err = "unknown wall layout \"" + w.shape + "\"";
        return false;
    }
    if (W != w.width || H != w.height) {
        err = "wall layout mismatch: computed " + std::to_string(W) + "x" + std::to_string(H) + " for \"" + app + "\"";
        return false;
    }
    for (int i = 0; i < n; i++) w.panels.push_back({"Virtual " + std::to_string(i + 1), virt[i]});
    if (bw > 0 && bh > 0) w.panels.push_back({"Screens", block});
    std::sort(w.panels.begin(), w.panels.end(), [](const Panel &a, const Panel &b) {
        return a.src.x != b.src.x ? a.src.x < b.src.x : a.src.y < b.src.y;
    });
    wall = std::move(w);
    return true;
}

std::vector<PanelPose> place_panels(const Wall &wall, const Placement &p, const std::vector<float> &scales) {
    const size_t n = wall.panels.size();
    std::vector<PanelPose> out(n);
    if (!n) return out;
    const float ppm = 1920.0f / p.meters_1920;  // pixels per metre at scale 1

    // Sizes, and the original centres in metres (x right, y down, wall centre = 0).
    std::vector<float> w(n), h(n), ox(n), cx(n), cy(n), ax(n, 0), ay(n, 0);
    for (size_t i = 0; i < n; i++) {
        const Rect &r = wall.panels[i].src;
        const float s = i < scales.size() && scales[i] > 0 ? scales[i] : 1.0f;
        w[i] = r.w / ppm * s;
        h[i] = r.h / ppm * s;
        ox[i] = (r.x + r.w / 2.0f - wall.width / 2.0f) / ppm;
    }
    // The anchor: the physical screens, else the panel nearest the middle.
    size_t anchor = 0;
    for (size_t i = 0; i < n; i++)
        if (wall.panels[i].name == "Screens" ||
            (wall.panels[anchor].name != "Screens" && std::fabs(ox[i]) < std::fabs(ox[anchor]))) anchor = i;

    // Rows: bands of panels whose vertical extents overlap in the wall.
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return wall.panels[a].src.y < wall.panels[b].src.y; });
    std::vector<std::vector<size_t>> rows;
    std::vector<std::pair<int, int>> bands;  // top, bottom in wall pixels
    std::vector<size_t> row_of(n);
    for (size_t i : order) {
        const Rect &r = wall.panels[i].src;
        if (rows.empty() || r.y >= bands.back().second) { rows.emplace_back(); bands.emplace_back(r.y, r.y + r.h); }
        rows.back().push_back(i);
        bands.back().second = std::max(bands.back().second, r.y + r.h);
        row_of[i] = rows.size() - 1;
    }

    // Horizontal: chain each row outwards from its fixed point.
    for (auto &row : rows) {
        std::sort(row.begin(), row.end(), [&](size_t a, size_t b) { return wall.panels[a].src.x < wall.panels[b].src.x; });
        int k = -1;  // index in `row` of a panel that stays centred
        float mid = 0;
        for (size_t j = 0; j < row.size(); j++) if (row[j] == anchor) k = int(j);
        if (k >= 0) {
            mid = ox[row[k]];
        } else {
            const int x0 = wall.panels[row.front()].src.x;
            const auto &last = wall.panels[row.back()].src;
            mid = ((x0 + last.x + last.w) / 2.0f - wall.width / 2.0f) / ppm;
            for (size_t j = 0; j < row.size(); j++) if (std::fabs(ox[row[j]] - mid) < 1e-3f) k = int(j);
        }
        float right_edge, left_edge;
        size_t first_right, last_left;  // chain from these
        if (k >= 0) {
            cx[row[k]] = mid;
            ax[row[k]] = 0;
            right_edge = mid + w[row[k]] / 2;
            left_edge = mid - w[row[k]] / 2;
            first_right = size_t(k) + 1;
            last_left = size_t(k);
        } else {  // split between the panels left and right of the middle
            right_edge = mid - p.gap / 2;
            left_edge = mid + p.gap / 2;
            first_right = 0;
            while (first_right < row.size() && ox[row[first_right]] < mid) first_right++;
            last_left = first_right;
        }
        for (size_t j = first_right; j < row.size(); j++) {
            const size_t i = row[j];
            cx[i] = right_edge + p.gap + w[i] / 2;
            ax[i] = -0.5f;  // left edge, against its neighbour
            right_edge = cx[i] + w[i] / 2;
        }
        for (size_t j = last_left; j-- > 0;) {
            const size_t i = row[j];
            cx[i] = left_edge - p.gap - w[i] / 2;
            ax[i] = 0.5f;
            left_edge = cx[i] - w[i] / 2;
        }
    }

    // Vertical (y down): the anchor's row keeps its centre; rows above sit on
    // top of it, rows below hang under it.
    const size_t ra = row_of[anchor];
    std::vector<float> row_h(rows.size(), 0);
    for (size_t r = 0; r < rows.size(); r++)
        for (size_t i : rows[r]) row_h[r] = std::max(row_h[r], h[i]);
    const float centre = ((bands[ra].first + bands[ra].second) / 2.0f - wall.height / 2.0f) / ppm;
    for (size_t i : rows[ra]) { cy[i] = centre; ay[i] = 0; }
    float edge = centre - row_h[ra] / 2;
    for (size_t r = ra; r-- > 0;) {
        const float bottom = edge - p.gap;
        for (size_t i : rows[r]) { cy[i] = bottom - h[i] / 2; ay[i] = -0.5f; }  // bottom edge fixed
        edge = bottom - row_h[r];
    }
    edge = centre + row_h[ra] / 2;
    for (size_t r = ra + 1; r < rows.size(); r++) {
        const float top = edge + p.gap;
        for (size_t i : rows[r]) { cy[i] = top + h[i] / 2; ay[i] = 0.5f; }  // top edge fixed
        edge = top + row_h[r];
    }

    for (size_t i = 0; i < n; i++) {
        PanelPose &pose = out[i];
        // Arc length cx along the cylinder -> angle to the right of forward.
        pose.theta = cx[i] / p.radius;
        pose.y = p.height - cy[i];
        const float c = std::cos(-pose.theta), s = std::sin(-pose.theta);
        pose.transform = vr::HmdMatrix34_t{{{c, 0, s, p.radius * std::sin(pose.theta)},
                                            {0, 1, 0, pose.y},
                                            {-s, 0, c, -p.radius * std::cos(pose.theta)}}};
        pose.width_m = w[i];
        pose.height_m = h[i];
        pose.anchor_x = ax[i];
        pose.anchor_y = ay[i];
    }
    return out;
}
