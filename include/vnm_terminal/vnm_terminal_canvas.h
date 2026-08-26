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
    Q_PROPERTY(qreal fontSize
        READ font_size WRITE set_font_size NOTIFY font_size_changed)
    Q_PROPERTY(int rows READ rows NOTIFY frame_changed)
    Q_PROPERTY(int columns READ columns NOTIFY frame_changed)
    Q_PROPERTY(qulonglong frameSequence READ frame_sequence NOTIFY frame_changed)

public:
    explicit VNM_TerminalCanvas(QQuickItem* parent = nullptr);
    ~VNM_TerminalCanvas() override;

    VNM_TerminalCanvas(const VNM_TerminalCanvas&) = delete;
    VNM_TerminalCanvas& operator=(const VNM_TerminalCanvas&) = delete;

    QString font_family() const;
    void set_font_family(const QString& font_family);

    qreal font_size() const;
    void set_font_size(qreal font_size);

    int rows() const;
    int columns() const;
    qulonglong frame_sequence() const;

    // The item copies and validates the immutable frame on its owning thread.
    // A rejected frame leaves the last accepted canvas installed.
    bool set_canvas_frame(
        std::shared_ptr<const vnm_terminal::Terminal_canvas_frame> frame);
    std::shared_ptr<const vnm_terminal::Terminal_canvas_frame>
        canvas_frame() const;

signals:
    void font_family_changed();
    void font_size_changed();
    void frame_changed();
    void cursor_blink_phase_changed(bool visible);

protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override;
    void releaseResources() override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    void refresh_render_state();
    void refresh_cursor_blink(bool was_enabled);
    void toggle_cursor_blink_phase();

    struct Private;
    std::unique_ptr<Private> m_private;
    QString                  m_font_family;
    qreal                    m_font_size = 13.0;
};
