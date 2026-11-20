#include "OverlayDialog.h"
#include "LocalFileDialog.h"
#include "integration/LocalPathPolicy.h"
#include "media/FrameComposer.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace openframe {
namespace {
const QStringList overlayNames{"Off",      "Safe Area", "Grid", "Center", "Dynamic Symmetry",
                               "Phi Grid", "Custom",    "Image"};
const QStringList shapeNames{"Line", "Rectangle", "Ellipse", "Polygon", "Text"};
QWidget *pairRow(QWidget *parent, QWidget *a, QWidget *b) {
    auto result = new QWidget(parent);
    auto layout = new QHBoxLayout(result);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(a);
    layout->addWidget(b);
    return result;
}
void swatch(QPushButton *button, const QColor &color) {
    button->setProperty("color", color);
    button->setText(color.name(QColor::HexArgb));
    QPixmap icon(28, 16);
    icon.fill(color);
    button->setIcon(QIcon(icon));
}
} // namespace

OverlayDialog::OverlayDialog(const OverlaySettings &value, QWidget *parent)
    : QDialog(parent), m_value(value) {
    setObjectName("overlayOptionsDialog");
    setWindowTitle("Viewer Overlay Options");
    resize(820, 680);
    auto layout = new QVBoxLayout(this);
    auto top = new QFormLayout;
    m_type = new QComboBox(this);
    m_type->setObjectName("overlayType");
    m_type->addItems(overlayNames);
    m_type->setCurrentIndex(int(value.type));
    top->addRow("Overlay", m_type);
    layout->addLayout(top);
    auto body = new QHBoxLayout;
    layout->addLayout(body, 1);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName("overlayTabs");
    body->addWidget(m_tabs, 1);
    auto side = new QVBoxLayout;
    body->addLayout(side);
    m_preview = new QLabel(this);
    m_preview->setObjectName("overlayPreview");
    m_preview->setFixedSize(320, 180);
    side->addWidget(m_preview);
    auto caption = new QLabel("Live overlay preview\nCheckerboard is a preview background.\n"
                              "Coordinates are fractions of the full viewer;\n"
                              "pan/zoom moves the complete composition.",
                              this);
    caption->setWordWrap(true);
    caption->setMaximumWidth(320);
    side->addWidget(caption);
    side->addStretch();
    auto formTab = [&](const QString &title) {
        auto scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto page = new QWidget(scroll);
        auto form = new QFormLayout(page);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        scroll->setWidget(page);
        m_tabs->addTab(scroll, title);
        return form;
    };
    auto number = [&](const QString &name, double low, double high, double initial,
                      int decimals = 3) {
        auto spin = new QDoubleSpinBox(this);
        spin->setObjectName(name);
        spin->setDecimals(decimals);
        spin->setRange(low, high);
        spin->setSingleStep(decimals ? .01 : 1);
        spin->setValue(initial);
        return spin;
    };
    auto guides = formTab("Guides");
    auto color = colorButton("overlayColor", value.color, [this](QColor c) { m_value.color = c; });
    auto opacity = number("overlayOpacity", 0, 1, value.opacity);
    auto lineWidth = number("overlayLineWidth", .1, 64, value.lineWidth, 1);
    guides->addRow("Line color", color);
    guides->addRow("Opacity", opacity);
    guides->addRow("Line width", lineWidth);
    auto columns = new QSpinBox(this), rows = new QSpinBox(this);
    columns->setObjectName("overlayGridColumns");
    rows->setObjectName("overlayGridRows");
    columns->setRange(1, 128);
    rows->setRange(1, 128);
    columns->setValue(value.gridColumns);
    rows->setValue(value.gridRows);
    guides->addRow("Grid columns / rows", pairRow(this, columns, rows));
    auto safeX = number("overlaySafeX", 0, .49, value.safeMargins.x());
    auto safeY = number("overlaySafeY", 0, .49, value.safeMargins.y());
    auto innerX = number("overlayInnerX", 0, .49, value.safeInnerMargins.x());
    auto innerY = number("overlayInnerY", 0, .49, value.safeInnerMargins.y());
    guides->addRow("Safe outer X / Y", pairRow(this, safeX, safeY));
    guides->addRow("Safe inner X / Y", pairRow(this, innerX, innerY));
    auto center = number("overlayCenterSize", 1, 1024, value.centerSize, 1);
    guides->addRow("Center marker size", center);
    auto guideNote = new QLabel("Safe margins are fractions from each edge. Widths and marker size "
                                "are measured against a 1920-pixel composition.",
                                this);
    guideNote->setWordWrap(true);
    guides->addRow(guideNote);
    connect(opacity, &QDoubleSpinBox::valueChanged, this, [this](double n) {
        m_value.opacity = n;
        refreshPreview();
    });
    connect(lineWidth, &QDoubleSpinBox::valueChanged, this, [this](double n) {
        m_value.lineWidth = n;
        refreshPreview();
    });
    connect(columns, &QSpinBox::valueChanged, this, [this](int n) {
        m_value.gridColumns = n;
        refreshPreview();
    });
    connect(rows, &QSpinBox::valueChanged, this, [this](int n) {
        m_value.gridRows = n;
        refreshPreview();
    });
    for (auto spin : {safeX, safeY, innerX, innerY})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [=, this] {
            m_value.safeMargins = {safeX->value(), safeY->value()};
            m_value.safeInnerMargins = {innerX->value(), innerY->value()};
            refreshPreview();
        });
    connect(center, &QDoubleSpinBox::valueChanged, this, [this](double n) {
        m_value.centerSize = n;
        refreshPreview();
    });

    auto custom = formTab("Custom");
    m_shapes = new QListWidget(this);
    m_shapes->setObjectName("overlayShapes");
    m_shapes->setMaximumHeight(112);
    custom->addRow(m_shapes);
    auto tools = new QWidget(this);
    auto actions = new QHBoxLayout(tools);
    actions->setContentsMargins(0, 0, 0, 0);
    for (auto item : {std::pair{"Add", "overlayAddShape"},
                      {"Copy", "overlayCopyShape"},
                      {"Delete", "overlayDeleteShape"},
                      {"Up", "overlayShapeUp"},
                      {"Down", "overlayShapeDown"}}) {
        auto button = new QPushButton(item.first, this);
        button->setObjectName(item.second);
        actions->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, item] {
            const QString command = item.first;
            if (command == "Add")
                addShape();
            else if (command == "Copy")
                addShape(true);
            else if (command == "Up")
                moveShape(-1);
            else if (command == "Down")
                moveShape(1);
            else {
                const int index = m_shapes->currentRow();
                if (index >= 0) {
                    m_value.shapes.removeAt(index);
                    refreshShapes(std::min(index, int(m_value.shapes.size()) - 1));
                    refreshPreview();
                }
            }
        });
    }
    custom->addRow(tools);
    m_shapeType = new QComboBox(this);
    m_shapeType->setObjectName("overlayShapeType");
    m_shapeType->addItems(shapeNames);
    custom->addRow("Shape", m_shapeType);
    m_visible = new QCheckBox("Visible", this);
    m_visible->setObjectName("overlayShapeVisible");
    custom->addRow(m_visible);
    m_points = new QPlainTextEdit(this);
    m_points->setObjectName("overlayShapePoints");
    m_points->setPlaceholderText("0.1, 0.1\n0.9, 0.9");
    m_points->setMaximumHeight(95);
    custom->addRow("Points (x, y per line)", m_points);
    auto pointsNote = new QLabel("Each coordinate is 0–1. Line/rectangle/ellipse: two points; "
                                 "polygon: three or more; text: one origin.",
                                 this);
    pointsNote->setWordWrap(true);
    custom->addRow(pointsNote);
    m_shapeColor = colorButton("overlayShapeColor", Qt::white, [this](QColor) { saveShape(); });
    m_shapeFill = colorButton("overlayShapeFill", Qt::transparent, [this](QColor) { saveShape(); });
    custom->addRow("Stroke / fill", pairRow(this, m_shapeColor, m_shapeFill));
    m_shapeWidth = number("overlayShapeWidth", .1, 64, 2, 1);
    m_shapeOpacity = number("overlayShapeOpacity", 0, 1, 1);
    custom->addRow("Width / opacity", pairRow(this, m_shapeWidth, m_shapeOpacity));
    m_text = new QPlainTextEdit(this);
    m_text->setObjectName("overlayShapeText");
    m_text->setMaximumHeight(70);
    custom->addRow("Text", m_text);
    m_fontSize = new QSpinBox(this);
    m_fontSize->setObjectName("overlayShapeFontSize");
    m_fontSize->setRange(1, 512);
    custom->addRow("Text size", m_fontSize);
    connect(m_shapes, &QListWidget::currentRowChanged, this, [this] { loadShape(); });
    connect(m_shapeType, &QComboBox::currentIndexChanged, this, [this] { saveShape(); });
    connect(m_visible, &QCheckBox::toggled, this, [this] { saveShape(); });
    connect(m_points, &QPlainTextEdit::textChanged, this, [this] { saveShape(); });
    connect(m_text, &QPlainTextEdit::textChanged, this, [this] { saveShape(); });
    connect(m_shapeWidth, &QDoubleSpinBox::valueChanged, this, [this] { saveShape(); });
    connect(m_shapeOpacity, &QDoubleSpinBox::valueChanged, this, [this] { saveShape(); });
    connect(m_fontSize, &QSpinBox::valueChanged, this, [this] { saveShape(); });

    auto image = formTab("Image");
    m_imagePath = new QLineEdit(value.imagePath, this);
    m_imagePath->setObjectName("overlayImagePath");
    auto browse = new QPushButton("Browse…", this);
    browse->setObjectName("overlayImageBrowse");
    image->addRow("Local image", pairRow(this, m_imagePath, browse));
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto file = LocalFileDialog::getOpenFileName(
            this, "Overlay Image", {},
            "Raster images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp)");
        if (!file.isEmpty())
            m_imagePath->setText(file);
    });
    connect(m_imagePath, &QLineEdit::textChanged, this, [this](const QString &path) {
        m_value.imagePath = path;
        refreshPreview();
    });
    auto x = number("overlayImageX", 0, 1, value.imageRect.x());
    auto y = number("overlayImageY", 0, 1, value.imageRect.y());
    auto width = number("overlayImageWidth", .001, 1, value.imageRect.width());
    auto height = number("overlayImageHeight", .001, 1, value.imageRect.height());
    image->addRow("Origin X / Y", pairRow(this, x, y));
    image->addRow("Width / height", pairRow(this, width, height));
    for (auto spin : {x, y, width, height})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [=, this] {
            m_value.imageRect = {x->value(), y->value(), width->value(), height->value()};
            refreshPreview();
        });
    auto imageOpacity = number("overlayImageOpacity", 0, 1, value.imageOpacity);
    image->addRow("Image opacity", imageOpacity);
    connect(imageOpacity, &QDoubleSpinBox::valueChanged, this, [this](double n) {
        m_value.imageOpacity = n;
        refreshPreview();
    });
    auto keep = new QCheckBox("Keep image aspect ratio", this);
    keep->setObjectName("overlayImageKeepAspect");
    keep->setChecked(value.imageKeepAspect);
    image->addRow(keep);
    connect(keep, &QCheckBox::toggled, this, [this](bool on) {
        m_value.imageKeepAspect = on;
        refreshPreview();
    });
    auto imageNote =
        new QLabel("Transparent pixels are preserved. The project links to this local "
                   "PNG, JPEG, BMP, TIFF or WebP file; keep it available when sharing the project.",
                   this);
    imageNote->setWordWrap(true);
    image->addRow(imageNote);
    m_error = new QLabel(this);
    m_error->setObjectName("overlayValidation");
    m_error->setWordWrap(true);
    m_error->setStyleSheet("color:#f3b96a");
    layout->addWidget(m_error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &OverlayDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto updateEnabled = [=, this] {
        const auto type = OverlayType(m_type->currentIndex());
        const bool guide =
            type != OverlayType::Off && type != OverlayType::Custom && type != OverlayType::Image;
        color->setEnabled(guide);
        lineWidth->setEnabled(guide);
        opacity->setEnabled(type != OverlayType::Off);
        columns->setEnabled(type == OverlayType::Grid);
        rows->setEnabled(type == OverlayType::Grid);
        for (auto spin : {safeX, safeY, innerX, innerY})
            spin->setEnabled(type == OverlayType::SafeArea);
        center->setEnabled(type == OverlayType::Center);
    };
    connect(m_type, &QComboBox::currentIndexChanged, this, [=, this](int index) {
        m_value.type = OverlayType(index);
        updateEnabled();
        m_tabs->setCurrentIndex(index == int(OverlayType::Custom)  ? 1
                                : index == int(OverlayType::Image) ? 2
                                                                   : 0);
        refreshPreview();
    });
    refreshShapes(value.shapes.isEmpty() ? -1 : 0);
    updateEnabled();
    m_tabs->setCurrentIndex(value.type == OverlayType::Custom  ? 1
                            : value.type == OverlayType::Image ? 2
                                                               : 0);
    refreshPreview();
}

QPushButton *OverlayDialog::colorButton(const QString &name, QColor initial,
                                        const std::function<void(QColor)> &changed) {
    auto button = new QPushButton(this);
    button->setObjectName(name);
    swatch(button, initial);
    connect(button, &QPushButton::clicked, this, [this, button, changed] {
        const auto color = QColorDialog::getColor(
            button->property("color").value<QColor>(), this, "Overlay Color",
            QColorDialog::ShowAlphaChannel | QColorDialog::DontUseNativeDialog);
        if (color.isValid()) {
            swatch(button, color);
            changed(color);
            refreshPreview();
        }
    });
    return button;
}
void OverlayDialog::refreshShapes(int selected) {
    m_loading = true;
    m_shapes->clear();
    for (int i = 0; i < m_value.shapes.size(); ++i)
        m_shapes->addItem(
            QString("%1  %2").arg(i + 1).arg(shapeNames.value(int(m_value.shapes[i].type))));
    m_shapes->setCurrentRow(selected);
    m_loading = false;
    loadShape();
}
void OverlayDialog::loadShape() {
    if (m_loading)
        return;
    m_loading = true;
    const int index = m_shapes->currentRow();
    const bool available = index >= 0 && index < m_value.shapes.size();
    for (auto widget : QList<QWidget *>{m_shapeType, m_visible, m_points, m_shapeColor, m_shapeFill,
                                        m_shapeWidth, m_shapeOpacity, m_text, m_fontSize})
        widget->setEnabled(available);
    if (available) {
        const auto &shape = m_value.shapes[index];
        m_shapeType->setCurrentIndex(int(shape.type));
        m_visible->setChecked(shape.visible);
        QStringList points;
        for (const auto &point : shape.points)
            points << QString::number(point.x(), 'g', 10) + ", " +
                          QString::number(point.y(), 'g', 10);
        m_points->setPlainText(m_pointText.value(shape.id, points.join('\n')));
        swatch(m_shapeColor, shape.color);
        swatch(m_shapeFill, shape.fill);
        m_shapeWidth->setValue(shape.width);
        m_shapeOpacity->setValue(shape.opacity);
        m_text->setPlainText(shape.text);
        m_fontSize->setValue(shape.fontSize);
        const bool text = shape.type == OverlayShapeType::Text;
        m_text->setEnabled(text);
        m_fontSize->setEnabled(text);
        m_shapeFill->setEnabled(shape.type != OverlayShapeType::Line && !text);
    } else {
        m_points->clear();
        m_text->clear();
    }
    m_validPoints = true;
    m_loading = false;
}
void OverlayDialog::saveShape() {
    if (m_loading || m_shapes->currentRow() < 0)
        return;
    auto &shape = m_value.shapes[m_shapes->currentRow()];
    m_pointText[shape.id] = m_points->toPlainText();
    QVector<QPointF> points;
    m_validPoints = true;
    const auto lines = m_points->toPlainText().split('\n', Qt::SkipEmptyParts);
    for (const auto &line : lines) {
        const auto values = line.trimmed().split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
        bool okX = false, okY = false;
        const double x = values.value(0).toDouble(&okX), y = values.value(1).toDouble(&okY);
        if (values.size() != 2 || !okX || !okY || !std::isfinite(x) || !std::isfinite(y) || x < 0 ||
            x > 1 || y < 0 || y > 1 || points.size() >= 4096) {
            m_validPoints = false;
            break;
        }
        points.append({x, y});
    }
    shape.type = OverlayShapeType(m_shapeType->currentIndex());
    const int minimum = shape.type == OverlayShapeType::Text      ? 1
                        : shape.type == OverlayShapeType::Polygon ? 3
                                                                  : 2;
    m_validPoints &= points.size() >= minimum;
    if (m_validPoints) {
        shape.points = points;
        m_invalidPointShapes.remove(shape.id);
    } else
        m_invalidPointShapes.insert(shape.id);
    shape.visible = m_visible->isChecked();
    shape.color = m_shapeColor->property("color").value<QColor>();
    shape.fill = m_shapeFill->property("color").value<QColor>();
    shape.width = m_shapeWidth->value();
    shape.opacity = m_shapeOpacity->value();
    shape.text = m_text->toPlainText();
    shape.fontSize = m_fontSize->value();
    m_shapes->currentItem()->setText(
        QString("%1  %2").arg(m_shapes->currentRow() + 1).arg(shapeNames[int(shape.type)]));
    const bool text = shape.type == OverlayShapeType::Text;
    m_text->setEnabled(text);
    m_fontSize->setEnabled(text);
    m_shapeFill->setEnabled(shape.type != OverlayShapeType::Line && !text);
    refreshPreview();
}
void OverlayDialog::addShape(bool duplicate) {
    if (m_value.shapes.size() >= 256) {
        m_error->setText("A custom overlay can contain at most 256 shapes.");
        return;
    }
    OverlayShape shape;
    if (duplicate && m_shapes->currentRow() >= 0)
        shape = m_value.shapes[m_shapes->currentRow()];
    else {
        shape.points = {{.1, .1}, {.9, .9}};
    }
    shape.id = newId();
    m_value.shapes.append(shape);
    m_type->setCurrentIndex(int(OverlayType::Custom));
    refreshShapes(int(m_value.shapes.size()) - 1);
    refreshPreview();
}
void OverlayDialog::moveShape(int direction) {
    const int from = m_shapes->currentRow(), to = from + direction;
    if (from < 0 || to < 0 || to >= m_value.shapes.size())
        return;
    m_value.shapes.swapItemsAt(from, to);
    refreshShapes(to);
    refreshPreview();
}
bool OverlayDialog::validate(QString *error) const {
    ViewerSettings viewer;
    viewer.overlay = m_value;
    if (!validateViewerSettings(viewer, error))
        return false;
    if (m_value.type == OverlayType::Custom) {
        if (!m_validPoints) {
            *error = "Enter valid 0–1 coordinate pairs and enough points for this shape.";
            return false;
        }
        int count = 0;
        for (const auto &shape : m_value.shapes) {
            count += int(shape.points.size());
            const int minimum = shape.type == OverlayShapeType::Text      ? 1
                                : shape.type == OverlayShapeType::Polygon ? 3
                                                                          : 2;
            if (m_invalidPointShapes.contains(shape.id) || shape.points.size() < minimum ||
                shape.text.size() > 16384 || count > 65536) {
                *error = "A shape has too few points, excessive text, or the total vertex limit is "
                         "exceeded.";
                return false;
            }
        }
    }
    if (m_value.type == OverlayType::Image) {
        const auto safe = LocalPathPolicy::validateInput(m_value.imagePath, error);
        if (safe.isEmpty())
            return false;
        const QList<QByteArray> formats{"png", "jpeg", "jpg", "bmp", "tiff", "tif", "webp"};
        const auto format = QFileInfo(safe).suffix().toLower().toLatin1();
        if (!formats.contains(format)) {
            *error = "Choose a PNG, JPEG, BMP, TIFF or WebP raster image.";
            return false;
        }
        QImageReader reader(safe, format);
        reader.setAutoDetectImageFormat(false);
        if (!reader.canRead()) {
            *error = "Choose a readable PNG, JPEG, BMP, TIFF or WebP raster image.";
            return false;
        }
    }
    return true;
}
void OverlayDialog::refreshPreview() {
    if (!m_error)
        return;
    QString error;
    const bool valid = validate(&error);
    m_error->setText(error);
    const qreal ratio = m_preview->devicePixelRatioF();
    QImage image(QSize(qRound(m_preview->width() * ratio), qRound(m_preview->height() * ratio)),
                 QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    const int tile = std::max(1, qRound(12 * ratio));
    for (int y = 0; y < image.height(); y += tile)
        for (int x = 0; x < image.width(); x += tile)
            painter.fillRect(x, y, tile, tile,
                             ((x / tile + y / tile) & 1) ? QColor("#353941") : QColor("#292d34"));
    painter.end();
    m_previewError.clear();
    if (valid) {
        try {
            FrameComposer::paintOverlay(image, m_value);
        } catch (const std::exception &exception) {
            m_previewError = QString::fromUtf8(exception.what());
            m_error->setText(m_previewError);
        }
    }
    auto pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(ratio);
    m_preview->setPixmap(pixmap);
}
void OverlayDialog::accept() {
    saveShape();
    refreshPreview();
    QString error;
    if (!validate(&error)) {
        m_error->setText(error);
        return;
    }
    if (!m_previewError.isEmpty())
        return;
    if (m_value.type == OverlayType::Image)
        m_value.imagePath = LocalPathPolicy::validateInput(m_value.imagePath, &error);
    QDialog::accept();
}
} // namespace openframe
