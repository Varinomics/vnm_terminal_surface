#include "vnm_terminal/internal/stage42_feature_flags.h"

#include <QByteArray>
#include <QtGlobal>

namespace vnm_terminal::internal {

namespace {

// Stage 4.2 isolation switches are benchmark toggles. Values are cached on
// first use, so A/B comparisons must use separate benchmark processes.
bool stage42_feature_enabled(const char* name, bool default_enabled)
{
    const QByteArray value = qgetenv(name).trimmed().toLower();
    if (value.isEmpty()) {
        return default_enabled;
    }

    return
        (value != "0"         &&
         value != "false"     &&
         value != "off"       &&
         value != "no");
}

stage42_feature_flags_t make_stage42_feature_flags()
{
    stage42_feature_flags_t flags;
    for (const stage42_feature_flag_metadata_t& metadata :
        stage42_feature_flag_metadata())
    {
        const bool default_enabled = flags.*(metadata.enabled_member);
        flags.*(metadata.enabled_member) =
            stage42_feature_enabled(metadata.environment_name, default_enabled);
    }
    return flags;
}

}

const stage42_feature_flag_metadata_array_t& stage42_feature_flag_metadata()
{
    static constexpr stage42_feature_flag_metadata_array_t metadata = {{
        {
            "model_ascii_direct_print",
            "VNM_TERMINAL_STAGE42_MODEL_ASCII_DIRECT_PRINT",
            &stage42_feature_flags_t::model_ascii_direct_print,
        },
        {
            "model_ascii_skip_simple_cell_clear",
            "VNM_TERMINAL_STAGE42_MODEL_ASCII_SKIP_SIMPLE_CELL_CLEAR",
            &stage42_feature_flags_t::model_ascii_skip_simple_cell_clear,
        },
        {
            "snapshot_inline_hyperlink_ids",
            "VNM_TERMINAL_STAGE42_SNAPSHOT_INLINE_HYPERLINK_IDS",
            &stage42_feature_flags_t::snapshot_inline_hyperlink_ids,
        },
        {
            "qsg_cached_internal_text_node",
            "VNM_TERMINAL_STAGE42_QSG_CACHED_INTERNAL_TEXT_NODE",
            &stage42_feature_flags_t::qsg_cached_internal_text_node,
        },
        {
            "qsg_trusted_ascii_unchecked_glyphs",
            "VNM_TERMINAL_STAGE42_QSG_TRUSTED_ASCII_UNCHECKED_GLYPHS",
            &stage42_feature_flags_t::qsg_trusted_ascii_unchecked_glyphs,
        },
        {
            "qsg_trusted_ascii_glyph_batching",
            "VNM_TERMINAL_STAGE42_QSG_TRUSTED_ASCII_GLYPH_BATCHING",
            &stage42_feature_flags_t::qsg_trusted_ascii_glyph_batching,
        },
        {
            "qsg_text_makeup_single_char_fast_path",
            "VNM_TERMINAL_STAGE42_QSG_TEXT_MAKEUP_SINGLE_CHAR_FAST_PATH",
            &stage42_feature_flags_t::qsg_text_makeup_single_char_fast_path,
        },
        {
            "qsg_ascii_resource_prefilter",
            "VNM_TERMINAL_STAGE42_QSG_ASCII_RESOURCE_PREFILTER",
            &stage42_feature_flags_t::qsg_ascii_resource_prefilter,
        },
        {
            "qsg_group_descriptor_eligibility",
            "VNM_TERMINAL_STAGE42_QSG_GROUP_DESCRIPTOR_ELIGIBILITY",
            &stage42_feature_flags_t::qsg_group_descriptor_eligibility,
        },
        {
            "qsg_monotonic_dirty_probe",
            "VNM_TERMINAL_STAGE42_QSG_MONOTONIC_DIRTY_PROBE",
            &stage42_feature_flags_t::qsg_monotonic_dirty_probe,
        },
        {
            "qsg_text_resource_descriptor_direct_compare",
            "VNM_TERMINAL_STAGE42_QSG_TEXT_RESOURCE_DESCRIPTOR_DIRECT_COMPARE",
            &stage42_feature_flags_t::qsg_text_resource_descriptor_direct_compare,
        },
        {
            "qsg_text_leaf_content_reuse",
            "VNM_TERMINAL_STAGE42_QSG_TEXT_LEAF_CONTENT_REUSE",
            &stage42_feature_flags_t::qsg_text_leaf_content_reuse,
        },
        {
            "qsg_row_slot_ordered_lookup",
            "VNM_TERMINAL_STAGE42_QSG_ROW_SLOT_ORDERED_LOOKUP",
            &stage42_feature_flags_t::qsg_row_slot_ordered_lookup,
        },
        {
            "qsg_descriptor_reuse_frame_key_independent",
            "VNM_TERMINAL_STAGE42_QSG_DESCRIPTOR_REUSE_FRAME_KEY_INDEPENDENT",
            &stage42_feature_flags_t::qsg_descriptor_reuse_frame_key_independent,
        },
        {
            "render_cell_row_cache",
            "VNM_TERMINAL_STAGE42_RENDER_CELL_ROW_CACHE",
            &stage42_feature_flags_t::render_cell_row_cache,
        },
        {
            "render_frame_sorted_row_sort_prefilter",
            "VNM_TERMINAL_STAGE42_RENDER_FRAME_SORTED_ROW_SORT_PREFILTER",
            &stage42_feature_flags_t::render_frame_sorted_row_sort_prefilter,
        },
    }};
    return metadata;
}

const stage42_feature_flags_t& stage42_feature_flags()
{
    static const stage42_feature_flags_t flags = make_stage42_feature_flags();
    return flags;
}

}
