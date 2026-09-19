#include <sa/ui/FieldDialog.h>

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <utility>

namespace sa::ui {

FieldDialog::FieldDialog(QWidget* parent, const QString& title, std::vector<Field> fields)
    : QDialog{parent} {
    setWindowTitle(title);

    auto* layout = new QVBoxLayout{this};
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    boxes_.reserve(fields.size());
    for (const Field& field : fields) {
        auto* box = new QDoubleSpinBox{this};
        box->setDecimals(field.decimals);
        // Range before value: setValue clamps to the range in force at the
        // time, so setting it first silently discards anything outside the
        // default 0-99.
        box->setRange(field.minimum, field.maximum);
        box->setSingleStep(field.step);
        box->setValue(field.value);
        if (!field.suffix.isEmpty()) {
            box->setSuffix(QStringLiteral(" ") + field.suffix);
        }
        box->setAlignment(Qt::AlignRight);
        form->addRow(field.label, box);
        boxes_.push_back(box);
    }
    layout->addLayout(form);

    auto* buttons =
        new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this};
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    if (!boxes_.empty()) {
        // The first field starts selected, so the common case is type-tab-type
        // rather than reach-for-the-mouse.
        boxes_.front()->setFocus();
        boxes_.front()->selectAll();
    }
}

std::vector<double> FieldDialog::values() const {
    std::vector<double> out;
    out.reserve(boxes_.size());
    for (const QDoubleSpinBox* box : boxes_) {
        out.push_back(box->value());
    }
    return out;
}

std::vector<double> FieldDialog::ask(QWidget* parent, const QString& title,
                                     std::vector<Field> fields) {
    FieldDialog dialog{parent, title, std::move(fields)};
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    return dialog.values();
}

} // namespace sa::ui
