#pragma once

#include "vnm_terminal/terminal_canvas_frame.h"

#include <QQuickItem>
#include <QString>
#include <memory>

class VNM_TerminalCanvas : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QString fontFamily
        READ font_family WRITE set_font_family NOTIFY font_family_changed)
    Q_PROPERTY(QString fontStyle
        READ font_style WRITE set_font_style NOTIFY font_style_changed)
    Q_PROPERTY(int fontWeight
        READ font_weight WRITE set_font_weight NOTIFY font_weight_changed)
    Q_PROPERTY(bool fontItalic
        READ font_italic WRITE set_font_italic NOTIFY font_italic_changed)
    Q_PROPERTY(qreal fontSize
        READ font_size WRITE set_font_size NOTIFY font_size_changed)
    Q_PROPERTY(bool authoritativeCellMetricsEnabled
        READ authoritative_cell_metrics_enabled
        WRITE set_authoritative_cell_metrics_enabled
        NOTIFY authoritative_cell_metrics_enabled_changed)
    Q_PROPERTY(int rows READ rows NOTIFY frame_changed)
    Q_PROPERTY(int columns READ columns NOTIFY frame_changed)
    Q_PROPERTY(qreal cellHeight READ cell_height NOTIFY frame_changed)
    Q_PROPERTY(bool contentExtentAvailable
        READ content_extent_available NOTIFY frame_changed)
    Q_PROPERTY(int contentBottomRowExclusive
        READ content_bottom_row_exclusive NOTIFY frame_changed)
    Q_PROPERTY(int scrollbackRows READ scrollback_rows NOTIFY frame_changed)
    Q_PROPERTY(int viewportOffsetFromTail
        READ viewport_offset_from_tail NOTIFY frame_changed)
    Q_PROPERTY(bool alternateScreen READ alternate_screen NOTIFY frame_changed)
    Q_PROPERTY(qulonglong frameSequence READ frame_sequence NOTIFY frame_changed)
    Q_PROPERTY(qulonglong frameGeneration
        READ frame_generation NOTIFY frame_changed)
    Q_PROPERTY(bool renderReady READ render_ready NOTIFY render_status_changed)
    Q_PROPERTY(qulonglong renderedFrameGeneration
        READ rendered_frame_generation NOTIFY render_status_changed)
    Q_PROPERTY(qulonglong renderedFrameSequence
        READ rendered_frame_sequence NOTIFY render_status_changed)
    Q_PROPERTY(qulonglong renderedPublicationGeneration
        READ rendered_publication_generation NOTIFY render_status_changed)
    Q_PROPERTY(QString renderError READ render_error NOTIFY render_error_changed)

public:
    explicit VNM_TerminalCanvas(QQuickItem* parent = nullptr);
    ~VNM_TerminalCanvas() override;

    VNM_TerminalCanvas(const VNM_TerminalCanvas&) = delete;
    VNM_TerminalCanvas& operator=(const VNM_TerminalCanvas&) = delete;

    QString font_family() const;
    void set_font_family(const QString& font_family);

    QString font_style() const;
    void set_font_style(const QString& font_style);

    int font_weight() const;
    void set_font_weight(int font_weight);

    bool font_italic() const;
    void set_font_italic(bool font_italic);

    qreal font_size() const;
    void set_font_size(qreal font_size);

    bool authoritative_cell_metrics_enabled() const;
    void set_authoritative_cell_metrics_enabled(bool enabled);

    int rows() const;
    int columns() const;
    qreal cell_height() const;
    bool content_extent_available() const;
    // Extent value accessors are meaningful only while availability is true;
    // otherwise they return neutral values.
    int content_bottom_row_exclusive() const;
    int scrollback_rows() const;
    int viewport_offset_from_tail() const;
    bool alternate_screen() const;
    qulonglong frame_sequence() const;
    qulonglong frame_generation() const;
    bool render_ready() const;
    qulonglong rendered_frame_generation() const;
    qulonglong rendered_frame_sequence() const;
    qulonglong rendered_publication_generation() const;
    QString render_error() const;

    // The item copies and validates the immutable frame on its owning thread.
    // A rejected frame leaves the last accepted canvas installed.
    bool set_canvas_frame(
        std::shared_ptr<const vnm_terminal::Terminal_canvas_frame> frame);
    std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>
        canvas_frame() const;

signals:
    void font_family_changed();
    void font_style_changed();
    void font_weight_changed();
    void font_italic_changed();
    void font_size_changed();
    void authoritative_cell_metrics_enabled_changed();
    void frame_changed();
    void render_status_changed();
    void render_error_changed();
    void cursor_blink_phase_changed(bool visible);

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override;
    void releaseResources() override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    void refresh_render_state();
    void refresh_render_status();
    void refresh_cursor_blink(bool was_enabled);
    void toggle_cursor_blink_phase();

    struct Private;
    std::unique_ptr<Private> m_private;
    QString                  m_font_family;
    QString                  m_font_style;
    int                      m_font_weight = 400;
    bool                     m_font_italic = false;
    qreal                    m_font_size = 13.0;
    bool                     m_authoritative_cell_metrics_enabled = false;
};
