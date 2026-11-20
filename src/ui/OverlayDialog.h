#pragma once
#include "core/Project.h"
#include <QDialog>
#include <functional>
#include <QSet>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace openframe {
// Edits a local value. Cancel never changes the project or undo history.
class OverlayDialog final : public QDialog {
  public:
    explicit OverlayDialog(const OverlaySettings &, QWidget *parent = nullptr);
    OverlaySettings settings() const {
        return m_value;
    }
    void accept() override;

  private:
    void refreshPreview();
    void refreshShapes(int selected = -1);
    void loadShape();
    void saveShape();
    void addShape(bool duplicate = false);
    void moveShape(int direction);
    bool validate(QString *error) const;
    QPushButton *colorButton(const QString &name, QColor initial,
                             const std::function<void(QColor)> &changed);
    OverlaySettings m_value;
    QComboBox *m_type, *m_shapeType;
    QTabWidget *m_tabs;
    QLabel *m_preview = nullptr, *m_error = nullptr;
    QListWidget *m_shapes;
    QPlainTextEdit *m_points, *m_text;
    QDoubleSpinBox *m_shapeWidth, *m_shapeOpacity;
    QSpinBox *m_fontSize;
    QCheckBox *m_visible;
    QPushButton *m_shapeColor, *m_shapeFill;
    QLineEdit *m_imagePath;
    QMap<QString, QString> m_pointText;
    QSet<QString> m_invalidPointShapes;
    QString m_previewError;
    bool m_loading = false, m_validPoints = true;
};
} // namespace openframe
