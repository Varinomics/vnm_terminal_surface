#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace vnm_terminal::internal {

struct qsg_atlas_warm_seed_string_t
{
    std::u16string_view text;
    std::string_view    family;
};

inline constexpr std::size_t k_qsg_atlas_warm_seed_string_budget = 16U;
inline constexpr std::size_t k_qsg_atlas_warm_seed_code_unit_budget = 384U;
inline constexpr std::size_t k_qsg_atlas_warm_seed_shaped_record_budget = 512U;

inline constexpr std::array<qsg_atlas_warm_seed_string_t, 11>
    k_qsg_atlas_warm_seed_strings = {{
        {
            u" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]"
            u"^_`abcdefghijklmnopqrstuvwxyz{|}~",
            "ascii_common",
        },
        {
            u"\u00a1\u00a2\u00a3\u00a4\u00a5\u00a7\u00a9\u00ab\u00ae\u00b0"
            u"\u00b1\u00b5\u00b6\u00bb\u00bf\u00c0\u00c4\u00c7\u00c9\u00d1"
            u"\u00d6\u00dc\u00df\u00e0\u00e4\u00e7\u00e9\u00f1\u00f6\u00fc",
            "latin_1",
        },
        {
            u"\u0100\u0101\u0104\u0105\u0106\u0107\u010c\u010d\u0112\u0113"
            u"\u0118\u0119\u0141\u0142\u0143\u0144\u015a\u015b\u0160\u0161"
            u"\u0179\u017a\u017d\u017e",
            "latin_extended_a",
        },
        {
            u"\u0391\u0392\u0393\u0394\u0395\u03a0\u03a3\u03a9\u03b1\u03b2"
            u"\u03b3\u03b4\u03b5\u03c0\u03c3\u03c9",
            "greek",
        },
        {
            u"\u0410\u0411\u0412\u0413\u0414\u0416\u0418\u0419\u041f\u042f"
            u"\u0430\u0431\u0432\u0433\u0434\u0436\u0438\u0439\u043f\u044f",
            "cyrillic",
        },
        {
            u"\u20ac\u20b9\u2190\u2191\u2192\u2193\u21d2\u2200\u2202\u2206"
            u"\u2208\u2211\u221a\u221e\u222b\u2248\u2260\u2264\u2265\u2302"
            u"\u2318\u2325\u23ce\u25a0\u25a1\u25b2\u25bc\u25c6\u25cb\u2605"
            u"\u2606\u2713\u2717",
            "symbols_currency_math",
        },
        {
            u"\u2500\u2502\u250c\u2510\u2514\u2518\u251c\u2524\u252c\u2534"
            u"\u253c\u2550\u2551\u2554\u2557\u255a\u255d\u2580\u2584\u2588"
            u"\u258c\u2590\u2591\u2592\u2593\u2596\u2597\u2598\u2599\u259a"
            u"\u259b\u259c\u259d\u259e\u259f\U0001fb00\U0001fb01\U0001fb02"
            u"\U0001fb03\u2800\u2801\u2802\u2803\u28ff",
            "terminal_graphics",
        },
        {
            u"\ue0a0\ue0a1\ue0a2\ue0b0\ue0b1\ue0b2\ue0b3\uf013\uf0a0\uf120"
            u"\uf126\uf17c",
            "pua_powerline_nerd",
        },
        {
            u"\u4e00\u4e2d\u65e5\u672c\u754c\u3042\u30a2\u30ab\u3131\u314f"
            u"\uac00\ud55c",
            "cjk_kana_hangul",
        },
        {
            u"e\u0301 n\u0303 a\u0308 o\u0302",
            "combining_clusters",
        },
        {
            u"\U0001f600\U0001f680 \u2615\ufe0f \u263a\ufe0f 1\ufe0f\u20e3 "
            u"\U0001f44b\U0001f3fd \U0001f469\u200d\U0001f4bb \U0001f468\u200d"
            u"\U0001f469\u200d\U0001f467",
            "emoji_clusters",
        },
    }};

} // namespace vnm_terminal::internal
