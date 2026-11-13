#!/usr/bin/env python3
"""Display geometry for iOS 16 at viva's native proportions (gpu/docs/PIANO-DISPLAY-NATIVO.md).

  viva-display-geometry.py [--hole-x PX] [--hole-y PX] [--hole-r PX] [--corner PX] [--json FILE]

From the panel (1080x2400, 69.55 x 154.56 mm) and the camera hole, computes in pixels and points (@3x):
the island (collapsed pill, SpringBoard's exclusion rect, inter-sensor region, edge insets), the status bar,
safe areas, home indicator, corner radius, the compatibility (letterbox) windows of third-party apps and the
kernel/ADT values. --json writes the values the SpringBoard tweak reads (preferences it.viva.display).
Defaults: measured on the phone 2026-11-13 (two photos against a link screenshot, gpu/docs/PIANO-DISPLAY-NATIVO.md 8.3).
"""
import argparse, json

PANEL_W, PANEL_H = 1080, 2400           # px
PANEL_MM = (69.55, 154.56)              # active area (Xiaomi dsi_k6t_38_0c_0a_dsc_vdo.c)
SCALE = 3.0

# iOS 16.7 (20H392) constants, read from the dyld cache
APPLE_PILL = (125.0, 36.667)            # SBUISystemApertureLayoutMetrics sensorRegionSize (14 Pro)
APPLE_EDGE = 11.333                     # _minimumScreenEdgeInset
APPLE_INTER = (213.333 - 196.5, 9.0)    # interSensorRegion: x offset from the screen centre, width (14 Pro, 393 pt)
STATUS_BAR_ISLAND = 53.667              # +[_UIStatusBarVisualProvider_DynamicSplit height]
STATUS_BAR_NOTCH = 47.0                 # +[_UIStatusBarVisualProvider_Split height]
SB_EDGE_INSETS = (0, 17, 0, 18)         # -[... DynamicSplit edgeInsets] top, leading, bottom, trailing (pt)
SB_CUTOUT_GAP = 8.0                     # -[... DynamicSplit cutoutEdgeInset]: items keep 8 pt from the island
HOME_INDICATOR_INSET = 34.0             # bottom safe area of every Face ID iPhone
CLASSIC = {3: ('4,7" (iPhone 8)', 375, 667), 5: ('5,8" (iPhone X)', 375, 812), 6: ('6,5" (iPhone XS Max)', 414, 896)}


def r3(v):
    return round(v * 3) / 3


def compute(a):
    pt_w, pt_h = PANEL_W / SCALE, PANEL_H / SCALE
    ppi = PANEL_W / (PANEL_MM[0] / 25.4)
    hx, hy, hr = a.hole_x / SCALE, a.hole_y / SCALE, a.hole_r / SCALE

    # Collapsed island: centred on the hole in x (SpringBoard centres it on the screen: the hole must be at
    # the centre, or the island is off it). Vertically: as close to the hole's centre as the top gap allows.
    top = a.island_top
    h = a.island_h if a.island_h else r3(2 * (hy - top))          # centred on the hole with the minimum gap
    h = max(h, 2 * (hr + a.margin))
    w = a.island_w if a.island_w else r3(h * APPLE_PILL[0] / APPLE_PILL[1])
    pill = (r3(pt_w / 2 - w / 2), top, w, h)
    pill_px = tuple(round(v * SCALE) for v in pill)
    margins = {'top': hy - hr - pill[1], 'bottom': pill[1] + pill[3] - (hy + hr),
               'left': hx - hr - pill[0], 'right': pill[0] + pill[2] - (hx + hr)}
    inter_x = r3(pt_w / 2 + APPLE_INTER[0] * w / APPLE_PILL[0])
    inter = (inter_x, pill[1], APPLE_INTER[1], pill[3])

    # status bar: DynamicSplit lays its items out on the exclusion rect (sensorAreaRect = _exclusionArea.rect),
    # centred on it vertically, cutoutEdgeInset (8 pt) from it, edgeInsets from the screen edges.
    side = (pt_w - w) / 2
    lead = side - SB_EDGE_INSETS[1] - SB_CUTOUT_GAP
    trail = side - SB_EDGE_INSETS[3] - SB_CUTOUT_GAP
    apple_side = (393 - APPLE_PILL[0]) / 2
    safe_top = max(STATUS_BAR_ISLAND, pill[1] + pill[3] + APPLE_EDGE)

    compat = {}
    for mode, (name, cw, ch) in CLASSIC.items():
        s = min(pt_w / cw, (pt_h - 0) / ch)
        compat[mode] = {'name': name, 'size_pt': (cw, ch), 'scale': s, 'shown_pt': (cw * s, ch * s),
                        'bars_pt': ((pt_w - cw * s) / 2, (pt_h - ch * s) / 2)}

    return {
        'panel': {'px': (PANEL_W, PANEL_H), 'pt': (pt_w, pt_h), 'scale': SCALE, 'ppi': ppi,
                  'mm_per_pt': 25.4 / ppi * SCALE},
        'hole': {'center_px': (a.hole_x, a.hole_y), 'radius_px': a.hole_r, 'center_pt': (hx, hy), 'radius_pt': hr,
                 'measured': a.measured},
        'corner_radius': {'px': a.corner, 'pt': a.corner / SCALE},
        'island': {'collapsed_pt': pill, 'collapsed_px': pill_px, 'radius_pt': pill[3] / 2,
                   'hole_margins_pt': margins, 'sensorRegionSize_pt': (w, h),
                   'minimumScreenEdgeInsets_pt': {'top': top, 'left': APPLE_EDGE, 'right': APPLE_EDGE},
                   'maximumWidth_pt': pt_w - 2 * APPLE_EDGE, 'interSensorRegion_pt': inter,
                   'exclusion_rect_px': pill_px},
        'status_bar': {'height_pt': STATUS_BAR_ISLAND, 'items_center_y_pt': pill[1] + pill[3] / 2,
                       'leading_width_pt': lead, 'trailing_width_pt': trail,
                       'apple_14pro_side_pt': apple_side - SB_EDGE_INSETS[1] - SB_CUTOUT_GAP},
        'safe_area_pt': {'portrait': {'top': safe_top, 'bottom': HOME_INDICATOR_INSET, 'left': 0, 'right': 0},
                         'landscape': {'top': 0, 'bottom': 21.0, 'left': safe_top, 'right': safe_top}},
        'compat': compat,
        'kernel_adt': {'canvas': (PANEL_W, PANEL_H), 'dot_pitch': round(ppi), 'display-corner-radius': (a.corner, 3),
                       'artwork-device-subtype': 2556, 'touch': '8640x19200 -> 1080x2400 (/8, no offset)'},
    }


def report(g):
    p, i, s = g['panel'], g['island'], g['status_bar']
    f = lambda t: '(' + ', '.join('%.2f' % v for v in t) + ')'
    print('Panel        %dx%d px = %.1fx%.1f pt @%gx, %.1f ppi (1 pt = %.3f mm)' % (*p['px'], *p['pt'], p['scale'],
                                                                               p['ppi'], p['mm_per_pt']))
    h = g['hole']
    print('Camera hole  centre %s px r %.1f px = centre %s pt r %.2f pt (%s)' % (
        h['center_px'], h['radius_px'], f(h['center_pt']), h['radius_pt'], 'measured' if h['measured'] else 'ESTIMATE'))
    print('Corners      radius %d px = %.2f pt' % (g['corner_radius']['px'], g['corner_radius']['pt']))
    print('Island       collapsed x,y,w,h %s pt = %s px, radius %.2f pt' % (f(i['collapsed_pt']), i['collapsed_px'],
                                                                         i['radius_pt']))
    print('             hole margins inside it (pt): %s' % ', '.join('%s %.2f' % kv for kv in i['hole_margins_pt'].items()))
    print('             sensorRegionSize %s pt, minimumScreenEdgeInsets top %.2f sides %.2f, maximumWidth %.2f pt' % (
        f(i['sensorRegionSize_pt']), i['minimumScreenEdgeInsets_pt']['top'], i['minimumScreenEdgeInsets_pt']['left'],
        i['maximumWidth_pt']))
    print('             interSensorRegion (privacy dot) %s pt' % f(i['interSensorRegion_pt']))
    print('             exclusion rect for UISDisplaySingleRectShape %s px' % (i['exclusion_rect_px'],))
    print('Status bar   height %.2f pt (fixed, DynamicSplit), items centred at y %.2f pt; room per side %.1f / %.1f pt '
          '(14 Pro: %.1f)' % (s['height_pt'], s['items_center_y_pt'], s['leading_width_pt'], s['trailing_width_pt'],
                              s['apple_14pro_side_pt']))
    for k, v in g['safe_area_pt'].items():
        print('Safe area    %-9s top %.2f bottom %.2f left %.2f right %.2f pt' % (k, v['top'], v['bottom'], v['left'],
                                                                             v['right']))
    for m, c in g['compat'].items():
        print('Compat %d     %-22s %dx%d pt x %.4f = %.1fx%.1f pt, bars %.1f pt each side, %.1f pt top/bottom' % (
            m, c['name'], *c['size_pt'], c['scale'], *c['shown_pt'], *c['bars_pt']))
    k = g['kernel_adt']
    print('Kernel/ADT   canvas %dx%d, dot pitch %d, display-corner-radius <%d %d>, artwork-device-subtype %d, touch %s'
          % (*k['canvas'], k['dot_pitch'], *k['display-corner-radius'], k['artwork-device-subtype'], k['touch']))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hole-x', type=float, default=543, help='camera hole centre, panel px (measured 2026-11-13 from photos)')
    ap.add_argument('--hole-y', type=float, default=50, help='camera hole centre, panel px (measured)')
    ap.add_argument('--hole-r', type=float, default=20, help='camera hole radius, px (measured 18 visible + 2 margin)')
    ap.add_argument('--corner', type=float, default=110, help='display corner radius, px (measured 110 +-10)')
    ap.add_argument('--estimate', dest='measured', action='store_false', help='the hole/corner values are guesses')
    ap.add_argument('--island-top', type=float, default=2.0, help='gap above the island, pt (default 2)')
    ap.add_argument('--island-h', type=float, default=0, help='island height, pt (default: centred on the hole)')
    ap.add_argument('--island-w', type=float, default=0, help='island width, pt (default: Apple proportions)')
    ap.add_argument('--margin', type=float, default=5.0, help='minimum margin around the hole, pt')
    ap.add_argument('--json', help='write the values for the tweak (it.viva.display) to this file')
    a = ap.parse_args()
    g = compute(a)
    report(g)
    if a.json:
        json.dump(g, open(a.json, 'w'), indent=1)
        print('->', a.json)


if __name__ == '__main__':
    main()
