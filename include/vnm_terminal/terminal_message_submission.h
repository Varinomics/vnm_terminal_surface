#pragma once

#include <QString>
#include <QtTypes>

namespace vnm_terminal {

inline constexpr qsizetype k_terminal_message_utf8_hard_limit_bytes =
    256 * 1024;

enum class Terminal_message_submission_outcome
{
    ACCEPTED,
    INVALID_UTF8,
    INVALID_MESSAGE,
    EMPTY_MESSAGE,
    MESSAGE_TOO_LARGE,
    NOT_RUNNING,
    QUEUE_LIMIT,
    BACKEND_REJECTED,
};

struct Terminal_message_submission_result
{
    Terminal_message_submission_outcome outcome =
        Terminal_message_submission_outcome::BACKEND_REJECTED;
    QString error;

    bool accepted() const
    {
        return outcome == Terminal_message_submission_outcome::ACCEPTED;
    }
};

} // namespace vnm_terminal
