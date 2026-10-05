#pragma once
// Exports on a worker that reads the document in place (commands::export_document; nothing edits it meanwhile): a view's
// hidden lines can take seconds on a big model, a STEP file too. Progress and Cancel in the status bar; a cancelled view
// stops. The window's status bar says where it went, a failure is a message box, and `done` gets the command's result or
// {"error": ...} ("cancelled" too), on the UI thread while the window lives. Throws when the document is busy.
#include <QString>
#include <functional>

#include "opad/json.hpp"

class AppDocument;
class JobRunner;
class QMainWindow;

void exportJob(AppDocument* doc, JobRunner* jobs, QMainWindow* window, const opad::json& args, const QString& out,
               std::function<void(const opad::json& result)> done = {});
