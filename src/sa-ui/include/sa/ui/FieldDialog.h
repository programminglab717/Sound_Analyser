#pragma once

#include <QDialog>
#include <QString>
#include <vector>

class QDoubleSpinBox;

namespace sa::ui {

/// A modal dialog of labelled numbers, asked for all at once.
///
/// Everything here used to be a chain of QInputDialog::getDouble calls, which
/// works for one or two values and falls apart at six: a compressor asked
/// through six consecutive modal dialogs cannot be adjusted, only re-entered
/// from the start, and cancelling the fifth throws away the four before it.
/// A form shows the whole setting at once, which is also the only way to see
/// that a threshold and a ratio go together.
///
/// Deliberately plain. This is not a control surface -- there is no curve to
/// drag and no meter moving beside it -- because those belong to a docked
/// panel that does not exist yet, and a dialog pretending to be one would be
/// in the way when it does.
class FieldDialog : public QDialog {
    Q_OBJECT

public:
    /// One row. `suffix` is shown inside the box, so a value reads "10.0 ms"
    /// rather than needing its unit in the label.
    struct Field {
        QString label;
        double value = 0.0;
        double minimum = 0.0;
        double maximum = 1.0;
        int decimals = 1;
        double step = 1.0;
        QString suffix;
    };

    FieldDialog(QWidget* parent, const QString& title, std::vector<Field> fields);

    /// The values as left by the user, in the order the fields were given.
    [[nodiscard]] std::vector<double> values() const;

    /// Show the dialog and return the values, or nothing if it was cancelled.
    ///
    /// The whole interaction in one call, because every caller wants exactly
    /// this and writing it out each time invites one of them to forget to
    /// check the result.
    [[nodiscard]] static std::vector<double> ask(QWidget* parent, const QString& title,
                                                 std::vector<Field> fields);

private:
    std::vector<QDoubleSpinBox*> boxes_;
};

} // namespace sa::ui
