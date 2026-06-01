#pragma once

#include <array>
#include <cstddef>

namespace vnm_terminal::internal {

struct stage42_feature_flags_t
{
    bool model_ascii_direct_print                  = true;
    bool model_ascii_skip_simple_cell_clear        = true;
    bool snapshot_inline_hyperlink_ids             = true;
    bool qsg_cached_internal_text_node             = true;
    bool qsg_trusted_ascii_unchecked_glyphs        = true;
    bool qsg_text_makeup_single_char_fast_path     = true;
    bool qsg_ascii_resource_prefilter              = true;
    bool qsg_group_descriptor_eligibility          = true;
    bool qsg_monotonic_dirty_probe                 = true;
    bool render_cell_row_cache                     = true;
};

inline constexpr std::size_t k_stage42_feature_flag_count = 10U;

struct stage42_feature_flag_metadata_t
{
    const char* key              = nullptr;
    const char* environment_name = nullptr;
    bool stage42_feature_flags_t::* enabled_member = nullptr;
};

using stage42_feature_flag_metadata_array_t =
    std::array<stage42_feature_flag_metadata_t, k_stage42_feature_flag_count>;

const stage42_feature_flag_metadata_array_t& stage42_feature_flag_metadata();
const stage42_feature_flags_t& stage42_feature_flags();

}
