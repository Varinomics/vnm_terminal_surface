#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vnm_terminal::internal {

constexpr int k_terminal_canvas_fixture_protocol_version = 1;

enum class Terminal_canvas_fixture_record_kind
{
    OUTPUT,
    EXPECT_INPUT,
    RESIZE,
    REPEAT_OUTPUT,
    CHECKPOINT,
    EXIT,
};

struct terminal_canvas_fixture_record_t
{
    Terminal_canvas_fixture_record_kind kind         = Terminal_canvas_fixture_record_kind::CHECKPOINT;
    std::string_view                    label;
    std::string_view                    payload_hex;
    int                                 rows         = 0;
    int                                 columns      = 0;
    int                                 repeat_count = 0;
    int                                 exit_code    = 0;
};

struct terminal_canvas_fixture_behavior_smoke_case_t
{
    std::string_view   name;
    std::string_view   payload_hex;
    int                repeat_count                  = 1;
};

struct terminal_canvas_fixture_shell_like_smoke_contract_t
{
    std::string_view   prompt;
    std::string_view   echo_command;
    std::string_view   echo_text;
    std::string_view   size_command;
    std::string_view   size_prefix;
    // POSIX only: reports the winsize pixel fields as <xpixel>x<ypixel>.
    std::string_view   pixel_size_command;
    std::string_view   pixel_size_prefix;
    // Sends CSI 16 t and reports the reply the terminal sends back, in hex.
    std::string_view   cell_size_query_command;
    std::string_view   cell_size_reply_prefix;
    std::string_view   stream_command;
    std::string_view   gated_stream_command;
    std::string_view   gated_stream_ready_output;
    std::string_view   gated_stream_resized_prefix;
    std::string_view   gated_stream_continue_command;
    std::string_view   wait_command;
    std::string_view   wait_output;
    std::string_view   exit_command;
    int                stream_count     = 64;
    int                stream_max_count = 256;
};

inline std::string_view terminal_canvas_fixture_kind_name(
    Terminal_canvas_fixture_record_kind kind)
{
    switch (kind) {
        case Terminal_canvas_fixture_record_kind::OUTPUT:        return "output";
        case Terminal_canvas_fixture_record_kind::EXPECT_INPUT:  return "expect-input";
        case Terminal_canvas_fixture_record_kind::RESIZE:        return "resize";
        case Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT: return "repeat-output";
        case Terminal_canvas_fixture_record_kind::CHECKPOINT:    return "checkpoint";
        case Terminal_canvas_fixture_record_kind::EXIT:          return "exit";
    }

    return {};
}

inline std::string_view terminal_canvas_fixture_scenario_name()
{
    return "terminal-canvas";
}

inline constexpr std::string_view k_terminal_canvas_fixture_enable_input_modes_label =
    "enable-input-modes";

inline constexpr terminal_canvas_fixture_shell_like_smoke_contract_t
terminal_canvas_fixture_shell_like_smoke_contract()
{
    return {
        "vnm$ ",
        "echo",
        "surface-ok",
        "size",
        "size ",
        "pixels",
        "pixels ",
        "cell-size-query",
        "cell-size-reply ",
        "stream",
        "stream-gated",
        "stream-gated-ready",
        "stream-gated-resized ",
        "stream-gated-continue",
        "wait",
        "waiting",
        "exit",
        64,
        256,
    };
}

inline const std::array<std::string_view, 15>& terminal_canvas_fixture_required_labels()
{
    static constexpr std::array<std::string_view, 15> labels = {
        "startup",
        "enter-alternate-screen",
        "prompt-editing-keys",
        "prompt-editing-keys-ack",
        "resize",
        "resize-report",
        "bracketed-paste",
        "bracketed-paste-ack",
        "focus-reporting",
        "focus-reporting-ack",
        "mouse-sgr-1006",
        "mouse-sgr-1006-ack",
        "reply-handling",
        "high-volume-streaming",
        "clean-exit",
    };

    return labels;
}

inline const std::vector<terminal_canvas_fixture_record_t>&
terminal_canvas_fixture_contract_script()
{
    static const std::vector<terminal_canvas_fixture_record_t> records = {
        {
            Terminal_canvas_fixture_record_kind::CHECKPOINT,
            "startup",
            {},
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "enter-alternate-screen",
            "1b5b3f3130343968",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "prompt",
            "7465726d3e20",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "prompt-editing-keys",
            "1b5b447f7465726d0d",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "prompt-editing-keys-ack",
            "696e7075742070726f6d70742d65646974696e672d6b657973206f6b0d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::RESIZE,
            "resize",
            {},
            33,
            120,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "resize-report",
            "726573697a65203333783132300d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            k_terminal_canvas_fixture_enable_input_modes_label,
            "1b5b3f32303034681b5b3f31303034681b5b3f31303030681b5b3f31303032681b5b3f3130303668",
            0,
            0,
            0,
            0,
        },
        // Bracketed paste is expected only after the fixture has enabled mode
        // 2004, because hosts should frame paste input from terminal state. The
        // body's line break is a carriage return rather than a line feed: that
        // is the byte a pasted line break has to reach the child as, because a
        // line feed resolves through the Windows console input parser to
        // Ctrl+Enter instead of Enter. See sanitize_paste_text().
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "bracketed-paste",
            "1b5b3230307e6c696e65310d6c696e65321b5b3230317e",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "bracketed-paste-ack",
            "696e70757420627261636b657465642d7061737465206f6b0d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "focus-reporting",
            "1b5b491b5b4f",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "focus-reporting-ack",
            "696e70757420666f6375732d7265706f7274696e67206f6b0d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "mouse-sgr-1006",
            "1b5b3c303b31323b384d1b5b3c303b31323b386d",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "mouse-sgr-1006-ack",
            "696e707574206d6f7573652d7367722d31303036206f6b0d0a",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "reply-handling",
            "1b5b32343b3830481b5b631b5b366e1b5b3f323030342470",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXPECT_INPUT,
            "reply-handling",
            "1b5b3f36313b34631b5b32343b3830521b5b3f323030343b312479",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::REPEAT_OUTPUT,
            "high-volume-streaming",
            "73747265616d2d726f770d0a",
            0,
            0,
            4096,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::OUTPUT,
            "leave-alternate-screen",
            "1b5b3f313034396c",
            0,
            0,
            0,
            0,
        },
        {
            Terminal_canvas_fixture_record_kind::EXIT,
            "clean-exit",
            {},
            0,
            0,
            0,
            0,
        },
    };

    return records;
}

inline const std::vector<terminal_canvas_fixture_behavior_smoke_case_t>&
terminal_canvas_fixture_behavior_smoke_cases()
{
    // Byte-output smokes shared by the fixture and tests. Resize/output ordering
    // needs an explicit model resize and is covered in the behavior smoke tests.
    static const std::vector<terminal_canvas_fixture_behavior_smoke_case_t> cases = {
        {
            "cursor-addressing",
            "411b5b323b3348421b5b313b31484344",
        },
        {
            "erase-line-screen",
            "6162636465661b5b313b33481b5b4b0d0a3132333435361b5b323b34481b5b314b",
        },
        {
            "alternate-screen-1049",
            "501b5b323b34481b5b3f3130343968414c541b5b333b38485a1b5b3f313034396c42",
        },
        {
            "unicode-width",
            "41e7958c4265cc8143e29da4efb88e44e29da4efb88f45f09f988046cea947",
        },
        {
            "output-burst",
            "30313233343536373839616263646566",
            512,
        },
        {
            "sgr-reset-interactions",
            "1b5b33316d411b5b316d421b5b346d431b5b32326d441b5b33396d451b5b306d46",
        },
        {
            "decstbm-scroll-region",
            "1b5b313b31483131311b5b323b31483232321b5b333b31483333331b5b343b31483434341b5b323b33721b5b333b31480a",
        },
        {
            "primary-scrollback-insert",
            "1b5b313b3148746f702d6f6e651b5b323b3148746f702d74776f1b5b333b3148"
            "766965771b5b343b314862656c6f771b5b353b314870726f6d70741b5b343b3572"
            "1b5b343b31481b4d1b5b721b5b313b34721b5b333b31480d0a484953541b5b72"
            "1b5b353b31481b5b313b34721b5b343b31480d0a4e4558541b5b721b5b353b3148",
        },
    };

    return cases;
}

// One case of the sixel cursor-sync gate: through the packaged ConPTY the
// child writes the payload, setup and one sixel image, and reports where
// OpenConsole left its cursor, which the session's model cursor must match.
struct Terminal_canvas_fixture_sixel_cursor_case
{
    std::string        name;
    std::string        payload;
    // Zero keeps the session's default retained history capacity.
    std::size_t        retained_history_capacity_bytes = 0U;
    // Recorded, not asserted: no adopted rule covers the case yet.
    bool               observation_only                = false;
};

// Written for a 24 x 80 screen and the fixed 10 x 20 cell OpenConsole places
// images on.
inline std::vector<Terminal_canvas_fixture_sixel_cursor_case>
terminal_canvas_fixture_sixel_cursor_cases()
{
    const auto cursor_to = [](int row, int column) {
        return "\x1b[" + std::to_string(row + 1) + ';' + std::to_string(column + 1) + 'H';
    };
    const auto image = [](std::string_view parameters, const std::string& data) {
        return "\x1bP" + std::string(parameters) + 'q' + data + "\x1b\\";
    };
    // Register 1 as red, then `rows` sixel rows of `width` copies of one sixel,
    // each row but the last followed by a graphics new line.
    const auto sixel_rows = [](int width, int rows, char sixel = '~') {
        std::string data = "#1;2;100;0;0";
        for (int row = 0; row < rows; ++row) {
            data += "#1!" + std::to_string(width) + sixel;
            if (row + 1 < rows) {
                data += '-';
            }
        }
        return data;
    };

    std::vector<Terminal_canvas_fixture_sixel_cursor_case> cases;

    // Images of one, two and five 20-pixel bands at each pixel aspect ratio,
    // from a row close enough to the bottom that the taller ones scroll. At
    // 5:1 one full sixel row is already two bands, so the one-band image
    // draws only the top pixel row of its sixels.
    struct band_case_t
    {
        const char* aspect;
        const char* parameters;
        int         rows;
        char        sixel;
        int         bands;
    };
    const band_case_t band_cases[] = {
        {"1:1", "7;1", 3,  '~', 1},
        {"1:1", "7;1", 6,  '~', 2},
        {"1:1", "7;1", 16, '~', 5},
        {"2:1", "0;1", 1,  '~', 1},
        {"2:1", "0;1", 3,  '~', 2},
        {"2:1", "0;1", 8,  '~', 5},
        {"3:1", "3;1", 1,  '~', 1},
        {"3:1", "3;1", 2,  '~', 2},
        {"3:1", "3;1", 5,  '~', 5},
        {"5:1", "2;1", 1,  '@', 1},
        {"5:1", "2;1", 1,  '~', 2},
        {"5:1", "2;1", 3,  '~', 5},
    };
    for (const band_case_t& band_case : band_cases) {
        const std::string data = sixel_rows(30, band_case.rows, band_case.sixel);
        cases.push_back({
            std::to_string(band_case.bands) + " band(s) at " + band_case.aspect,
            cursor_to(20, 5) + image(band_case.parameters, data),
        });
    }

    cases.push_back({
        "raster attributes 1;1 over P1 0",
        cursor_to(20, 5) + image("0;1", "\"1;1;30;18" + sixel_rows(30, 3)),
    });
    cases.push_back({
        "raster attributes 3;2 round up to 2:1",
        cursor_to(20, 5) + image("7;1", "\"3;2;30;36" + sixel_rows(30, 3)),
    });
    cases.push_back({
        "trailing graphics new line",
        cursor_to(20, 5) + image("0;1", sixel_rows(30, 2) + '-'),
    });
    cases.push_back({
        "DECSDM set",
        "\x1b[?80h" + cursor_to(10, 10) + image("7;1", sixel_rows(30, 16)),
    });
    cases.push_back({
        "DECSDM reset",
        "\x1b[?80l" + cursor_to(10, 10) + image("7;1", sixel_rows(30, 16)),
    });
    cases.push_back({
        "bottom margin of the full screen",
        cursor_to(23, 0) + image("7;1", sixel_rows(30, 16)),
    });
    cases.push_back({
        "bottom margin of a scroll region",
        "\x1b[5;15r" + cursor_to(14, 0) + image("7;1", sixel_rows(30, 16)),
    });
    cases.push_back({
        "origin below the scroll region",
        "\x1b[5;15r" + cursor_to(20, 3) + image("7;1", sixel_rows(30, 6)),
    });
    cases.push_back({
        "origin above the scroll region, overflowing it",
        "\x1b[10;15r" + cursor_to(5, 0) + image("7;1", sixel_rows(30, 40)),
        0U,
        true,
    });
    cases.push_back({
        "empty image at the bottom",
        cursor_to(23, 0) + image("0;1", "---"),
    });
    // 200 x 204 pixels is 163200 bytes, over the 131072 byte cap of a 1 MiB
    // retained history ring; OpenConsole shows it whole.
    cases.push_back({
        "image over the decoded-size cap",
        cursor_to(5, 0) + image("7;1", sixel_rows(200, 34)),
        1024U * 1024U,
    });
    cases.push_back({
        "CAN in the middle of an image",
        cursor_to(10, 5) + "\x1bP7;1q" + sixel_rows(30, 3) + '\x18',
    });
    cases.push_back({
        "pending wrap and one band, then text",
        cursor_to(10, 0) + std::string(80, 'x') + image("7;1", sixel_rows(30, 3)) + 'X',
    });

    return cases;
}

}
