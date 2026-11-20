#include "Theme.h"
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleFactory>
namespace openframe {
QIcon uiIcon(const QString &name, const QColor &color) {
    QPixmap pix(40, 40);
    pix.fill(Qt::transparent);
    pix.setDevicePixelRatio(2);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    auto line = [&](qreal a, qreal b, qreal c, qreal d) {
        p.drawLine(QPointF(a, b), QPointF(c, d));
    };
    if (name == "play" || name == "reverse") {
        QPolygonF poly;
        if (name == "play")
            poly << QPointF(7, 4) << QPointF(16, 10) << QPointF(7, 16);
        else
            poly << QPointF(14, 4) << QPointF(5, 10) << QPointF(14, 16);
        p.setBrush(color);
        p.drawPolygon(poly);
    } else if (name == "pause") {
        p.setBrush(color);
        p.drawRect(QRectF(6, 4, 2.5, 12));
        p.drawRect(QRectF(12, 4, 2.5, 12));
    } else if (name == "step" || name == "back" || name == "end" || name == "start") {
        bool rev = name == "back" || name == "start";
        p.save();
        if (rev) {
            p.translate(20, 0);
            p.scale(-1, 1);
        }
        QPolygonF poly;
        poly << QPointF(5, 5) << QPointF(12, 10) << QPointF(5, 15);
        p.setBrush(color);
        p.drawPolygon(poly);
        line(14, 5, 14, 15);
        if (name == "end" || name == "start")
            line(17, 5, 17, 15);
        p.restore();
    } else if (name == "folder") {
        QPainterPath path;
        path.moveTo(3, 6);
        path.lineTo(3, 4);
        path.lineTo(8, 4);
        path.lineTo(10, 6);
        path.lineTo(17, 6);
        path.lineTo(17, 16);
        path.lineTo(3, 16);
        path.closeSubpath();
        p.drawPath(path);
        line(3, 8, 17, 8);
    } else if (name == "import") {
        p.drawRect(QRectF(3, 12, 14, 5));
        line(10, 2, 10, 12);
        line(6, 8, 10, 12);
        line(14, 8, 10, 12);
    } else if (name == "save") {
        p.drawRect(QRectF(3, 3, 14, 14));
        p.drawRect(QRectF(6, 3, 7, 5));
        p.drawRect(QRectF(6, 11, 8, 6));
    } else if (name == "timeline") {
        for (int i = 0; i < 3; ++i)
            p.drawRoundedRect(QRectF(3 + i * 2, 4 + i * 4, 11 - i * 2, 2), .5, .5);
    } else if (name == "media") {
        p.drawRect(QRectF(3, 3, 14, 14));
        line(6, 3, 6, 17);
        line(14, 3, 14, 17);
        for (int y = 6; y < 17; y += 4) {
            line(3, y, 6, y);
            line(14, y, 17, y);
        }
    } else if (name == "bookmark") {
        QPainterPath q;
        q.moveTo(5, 3);
        q.lineTo(15, 3);
        q.lineTo(15, 17);
        q.lineTo(10, 13);
        q.lineTo(5, 17);
        q.closeSubpath();
        p.drawPath(q);
    } else if (name == "note") {
        p.drawRoundedRect(QRectF(3, 3, 14, 14), 1, 1);
        for (int y = 7; y < 15; y += 3)
            line(6, y, 14, y);
    } else if (name == "pencil") {
        QPolygonF q;
        q << QPointF(4, 14) << QPointF(13, 4) << QPointF(16, 7) << QPointF(7, 17) << QPointF(3, 18);
        p.drawPolygon(q);
        line(11, 6, 14, 9);
    } else if (name == "eyedropper") {
        QPolygonF tube;
        tube << QPointF(4, 15) << QPointF(11, 6) << QPointF(14, 9) << QPointF(7, 17)
             << QPointF(3, 18);
        p.drawPolygon(tube);
        line(10, 5, 15, 10);
        p.drawEllipse(QRectF(13, 2, 4, 4));
    } else if (name == "laser") {
        p.drawEllipse(QRectF(7, 7, 6, 6));
        line(10, 2, 10, 5);
        line(10, 15, 10, 18);
        line(2, 10, 5, 10);
        line(15, 10, 18, 10);
        line(4, 4, 6, 6);
        line(14, 14, 16, 16);
    } else if (name == "arrow") {
        line(4, 16, 16, 4);
        line(9, 4, 16, 4);
        line(16, 4, 16, 11);
    } else if (name == "rectangle")
        p.drawRect(QRectF(3, 5, 14, 10));
    else if (name == "ellipse")
        p.drawEllipse(QRectF(3, 4, 14, 12));
    else if (name == "text") {
        line(4, 4, 16, 4);
        line(10, 4, 10, 17);
        line(7, 17, 13, 17);
    } else if (name == "erase") {
        QPolygonF q;
        q << QPointF(4, 13) << QPointF(12, 4) << QPointF(17, 9) << QPointF(10, 17)
          << QPointF(8, 17);
        p.drawPolygon(q);
        line(7, 10, 13, 15);
    } else if (name == "undo" || name == "redo") {
        p.save();
        if (name == "redo") {
            p.translate(20, 0);
            p.scale(-1, 1);
        }
        p.drawArc(QRectF(5, 5, 12, 11), -60 * 16, 230 * 16);
        line(3, 6, 8, 6);
        line(3, 6, 3, 1);
        p.restore();
    } else if (name == "delete") {
        line(4, 5, 16, 5);
        line(8, 3, 12, 3);
        p.drawRect(QRectF(6, 5, 8, 12));
        line(9, 8, 9, 14);
        line(12, 8, 12, 14);
    } else if (name == "plus") {
        line(10, 4, 10, 16);
        line(4, 10, 16, 10);
    } else if (name == "minus")
        line(4, 10, 16, 10);
    else if (name == "grid") {
        p.drawRect(QRectF(3, 3, 14, 14));
        line(10, 3, 10, 17);
        line(3, 10, 17, 10);
    } else if (name == "split") {
        p.drawRect(QRectF(3, 4, 14, 12));
        line(10, 4, 10, 16);
    } else if (name == "ab") {
        p.setFont(QFont("Segoe UI", 9, QFont::DemiBold));
        p.drawText(QRectF(0, 0, 20, 20), Qt::AlignCenter, "A/B");
    } else if (name == "zoom") {
        p.drawEllipse(QRectF(3, 3, 10, 10));
        line(12, 12, 17, 17);
        line(8, 5, 8, 11);
        line(5, 8, 11, 8);
    } else if (name == "fit") {
        line(3, 8, 3, 3);
        line(3, 3, 8, 3);
        line(12, 3, 17, 3);
        line(17, 3, 17, 8);
        line(3, 12, 3, 17);
        line(3, 17, 8, 17);
        line(12, 17, 17, 17);
        line(17, 17, 17, 12);
    } else if (name == "rotate") {
        p.drawArc(QRectF(4, 4, 12, 12), 20 * 16, 290 * 16);
        line(17, 4, 17, 9);
        line(17, 9, 12, 9);
    } else if (name == "sound") {
        QPolygonF q;
        q << QPointF(3, 8) << QPointF(6, 8) << QPointF(10, 4) << QPointF(10, 16) << QPointF(6, 12)
          << QPointF(3, 12);
        p.drawPolygon(q);
        p.drawArc(QRectF(7, 5, 9, 10), -70 * 16, 140 * 16);
    } else if (name == "loop") {
        p.drawArc(QRectF(3, 4, 14, 12), -20 * 16, 160 * 16);
        p.drawArc(QRectF(3, 4, 14, 12), 160 * 16, 160 * 16);
        line(3, 4, 3, 9);
        line(3, 9, 8, 9);
        line(17, 11, 17, 16);
        line(12, 11, 17, 11);
    } else if (name == "settings") {
        p.drawEllipse(QRectF(6, 6, 8, 8));
        for (int i = 0; i < 8; ++i) {
            p.save();
            p.translate(10, 10);
            p.rotate(i * 45);
            line(0, 6, 0, 8);
            p.restore();
        }
    } else if (name == "up" || name == "down") {
        bool up = name == "up";
        line(5, up ? 12 : 8, 10, up ? 7 : 13);
        line(10, up ? 7 : 13, 15, up ? 12 : 8);
    } else if (name == "eye") {
        p.drawEllipse(QRectF(3, 6, 14, 8));
        p.drawEllipse(QRectF(8, 8, 4, 4));
    } else {
        p.drawRoundedRect(QRectF(4, 4, 12, 12), 2, 2);
    }
    return QIcon(pix);
}
void applyDarkTheme(QApplication &app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette pal;
    pal.setColor(QPalette::Window, QColor("#303235"));
    pal.setColor(QPalette::WindowText, QColor("#d2d4d7"));
    pal.setColor(QPalette::Base, QColor("#25272a"));
    pal.setColor(QPalette::AlternateBase, QColor("#2a2c2f"));
    pal.setColor(QPalette::Text, QColor("#d2d4d7"));
    pal.setColor(QPalette::PlaceholderText, QColor("#8e969e"));
    pal.setColor(QPalette::Button, QColor("#3b3e42"));
    pal.setColor(QPalette::ButtonText, QColor("#d2d4d7"));
    pal.setColor(QPalette::Highlight, QColor("#496b82"));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::ToolTipBase, QColor("#202226"));
    pal.setColor(QPalette::ToolTipText, QColor("#e0e0e0"));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor("#73777c"));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#73777c"));
    app.setPalette(pal);
    app.setFont(QFont("Segoe UI", 9));
    app.setStyleSheet(QString::fromLatin1(R"(
QMainWindow::separator { background:#242629; width:4px; height:4px; }
QDockWidget::title { background:#373a3e; padding:5px 8px; font-weight:600; }
QToolBar { border:0; background:#303235; spacing:2px; padding:3px; }
QToolBar::separator { width:1px; background:#505357; margin:5px 4px; }
QToolButton { border:1px solid transparent; border-radius:3px; padding:4px; }
QToolButton:hover { background:#44484d; border-color:#555a61; }
QToolButton:checked { background:#4b6679; border-color:#6d8a9d; }
QPushButton { padding:5px 13px; border:1px solid #55595e; border-radius:3px; background:#3c4045; }
QPushButton:hover { background:#4a5057; }
QPushButton:default { border-color:#7b9ab2; }
QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit, QKeySequenceEdit { min-height:22px; border:1px solid #51555a; border-radius:2px; padding:1px 5px; background:#292c30; }
QComboBox { padding-right:22px; }
QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:top right; width:18px; border:0; }
QComboBox::down-arrow { image:url(:/openframe/arrow-down.svg); width:10px; height:8px; }
QComboBox::down-arrow:disabled { image:url(:/openframe/arrow-down-disabled.svg); }
QSpinBox, QDoubleSpinBox { padding-right:19px; }
QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin:border; subcontrol-position:top right; width:17px; border-left:1px solid #454a51; background:#34383d; }
QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin:border; subcontrol-position:bottom right; width:17px; border-left:1px solid #454a51; border-top:1px solid #454a51; background:#34383d; }
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover { background:#48515b; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image:url(:/openframe/arrow-up.svg); width:9px; height:7px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image:url(:/openframe/arrow-down.svg); width:9px; height:7px; }
QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled, QSpinBox::up-arrow:off, QDoubleSpinBox::up-arrow:off { image:url(:/openframe/arrow-up-disabled.svg); }
QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled, QSpinBox::down-arrow:off, QDoubleSpinBox::down-arrow:off { image:url(:/openframe/arrow-down-disabled.svg); }
QTreeView, QTableView, QListView { border:1px solid #242629; outline:0; }
QTreeView::item, QListView::item { padding:4px 2px; }
QHeaderView::section { background:#34373b; border:0; border-right:1px solid #25272a; padding:5px; }
QTabWidget::pane { border:1px solid #242629; }
QTabBar::tab { background:#2b2d30; padding:6px 14px; border:1px solid #242629; }
QTabBar::tab:selected { background:#3a3d41; border-bottom-color:#3a3d41; }
QMenu { border:1px solid #51555b; padding:4px; }
QMenu::item { padding:5px 26px 5px 23px; }
QMenu::item:selected { background:#496b82; }
QMenu::separator { height:1px; background:#52565c; margin:4px 7px; }
QStatusBar { background:#292b2e; }
QGroupBox { border:1px solid #51555a; border-radius:3px; margin-top:12px; padding-top:8px; }
QGroupBox::title { subcontrol-origin:margin; left:9px; padding:0 4px; }
QScrollBar:vertical { width:12px; background:#26282b; }
QScrollBar::handle:vertical { background:#555b62; min-height:24px; border:3px solid #26282b; border-radius:5px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
QSlider::groove:horizontal { height:4px; background:#1e2023; border-radius:2px; }
QSlider::handle:horizontal { width:10px; margin:-4px 0; border-radius:3px; background:#9cabb5; }
QToolTip { padding:5px; border:1px solid #606872; }
)"));
}
} // namespace openframe
