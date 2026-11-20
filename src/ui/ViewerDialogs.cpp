#include "ViewerDialogs.h"
#include "core/ReviewSession.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace openframe {
namespace {
QDoubleSpinBox *number(QFormLayout *form, const QString &label, const QString &name, double initial,
                       double minimum, double maximum, int decimals = 2) {
    auto field = new QDoubleSpinBox(form->parentWidget());
    field->setObjectName(name);
    field->setDecimals(decimals);
    field->setRange(minimum, maximum);
    field->setValue(initial);
    field->setKeyboardTracking(false);
    form->addRow(label, field);
    return field;
}
QDialogButtonBox *buttons(QDialog &dialog, QVBoxLayout *layout) {
    auto result = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(result);
    QObject::connect(result, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(result, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return result;
}
} // namespace
void editWipeSettings(ReviewSession *session, QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Wipe Options");
    dialog.setObjectName("wipeOptionsDialog");
    dialog.setMinimumWidth(430);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    const auto initial = session->project().viewer;
    auto x = number(form, "Horizontal pivot", "wipePivotX", initial.wipe * 100, 0, 100);
    auto y = number(form, "Vertical pivot", "wipePivotY", initial.wipePivotY * 100, 0, 100);
    x->setSuffix(" %");
    y->setSuffix(" %");
    auto angle = number(form, "Rotation", "wipeAngle", initial.wipeAngleDegrees, 0, 360);
    angle->setSuffix("°");
    auto hint = new QLabel("Drag the divider to move it, the pivot to reposition it, or the "
                           "round handle to rotate. Ctrl temporarily disables snapping. Hold Alt "
                           "to adjust the wipe while annotation tools are enabled.",
                           &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto reset = new QPushButton("Reset: centered vertical wipe", &dialog);
    reset->setObjectName("wipeReset");
    layout->addWidget(reset);
    auto state = new QLabel(&dialog);
    state->setWordWrap(true);
    layout->addWidget(state);
    auto box = buttons(dialog, layout);
    const auto token = session->beginViewerPreview();
    auto preview = [&] {
        auto settings = initial;
        settings.mode = ViewerMode::Wipe;
        settings.wipe = x->value() / 100;
        settings.wipePivotY = y->value() / 100;
        settings.wipeAngleDegrees = angle->value() == 360 ? 0 : angle->value();
        if (!session->previewViewerSettings(token, settings)) {
            state->setText(
                "The project changed. Close this dialog and reopen it to adjust the wipe.");
            box->button(QDialogButtonBox::Ok)->setEnabled(false);
        }
    };
    for (auto control : {x, y, angle})
        QObject::connect(control, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                         [&](double) { preview(); });
    QObject::connect(reset, &QPushButton::clicked, &dialog, [&] {
        x->setValue(50);
        y->setValue(50);
        angle->setValue(0);
        preview();
    });
    if (dialog.exec() == QDialog::Accepted) {
        preview();
        session->commitViewerPreview(token, "Adjust wipe");
    }
    session->clearViewerPreview(token);
}
void editColorSettings(ReviewSession *session, QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle("HDR / Display Color");
    dialog.setObjectName("colorSettingsDialog");
    dialog.setMinimumWidth(460);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    layout->addLayout(form);
    const auto initial = session->project().viewer;
    auto tone = new QCheckBox("Tone map HDR to the display range", &dialog);
    tone->setObjectName("hdrToneMapping");
    tone->setChecked(initial.color.hdrToneMapping);
    form->addRow(tone);
    auto source = number(form, "Source peak (0 = metadata / 1000)", "sourcePeakNits",
                         initial.color.sourcePeakNits, 0, 10000);
    auto white = number(form, "Reference white", "referenceWhiteNits",
                        initial.color.referenceWhiteNits, 1, 1000);
    auto display =
        number(form, "Display peak", "displayPeakNits", initial.color.displayPeakNits, 1, 1000);
    for (auto field : {source, white, display})
        field->setSuffix(" cd/m²");
    auto transition = number(form, "Mobius transition", "mobiusTransition",
                             initial.color.mobiusTransition, 0, .95, 3);
    auto description = new QLabel("The viewer is displayed as 8-bit sRGB. HDR is converted using "
                                  "the source color metadata and the selected luminance settings. "
                                  "High-depth export is configured separately in Export to File.",
                                  &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto state = new QLabel(&dialog);
    state->setWordWrap(true);
    layout->addWidget(state);
    auto box = buttons(dialog, layout);
    const auto token = session->beginViewerPreview();
    auto preview = [&] {
        auto settings = initial;
        settings.color = {tone->isChecked(), source->value(), white->value(), display->value(),
                          transition->value()};
        QString error;
        const bool valid = validateColorSettings(settings.color, &error);
        const bool applied = valid && session->previewViewerSettings(token, settings);
        box->button(QDialogButtonBox::Ok)->setEnabled(applied);
        state->setText(
            !valid    ? error
            : applied ? QString()
                      : "The project changed. Close this dialog and reopen it to adjust color.");
        transition->setEnabled(tone->isChecked());
    };
    for (auto field : {source, white, display, transition})
        QObject::connect(field, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
                         [&](double) { preview(); });
    QObject::connect(tone, &QCheckBox::toggled, &dialog, [&](bool) { preview(); });
    if (dialog.exec() == QDialog::Accepted) {
        preview();
        if (session->displayedViewerSettings().color != initial.color)
            session->commitViewerPreview(token, "Adjust display color");
    }
    session->clearViewerPreview(token);
}
} // namespace openframe
