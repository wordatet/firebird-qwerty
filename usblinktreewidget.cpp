#include <QDragEnterEvent>
#include <QFileDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QWidgetAction>

#include "usblinktreewidget.h"

#include "core/usblink_queue.h"

static USBLinkTreeWidget *usblink_tree = nullptr;

USBLinkTreeWidget::USBLinkTreeWidget(QWidget *parent) : QTreeWidget(parent) {
  connect(this, SIGNAL(customContextMenuRequested(QPoint)), this,
          SLOT(customContextMenuRequested(QPoint)));
  connect(this, SIGNAL(itemChanged(QTreeWidgetItem *, int)), this,
          SLOT(dataChangedHandler(QTreeWidgetItem *, int)));
  // This is a Qt::BlockingQueuedConnection as the usblink_dirlist_* family of
  // functions needs to enumerate over the items directly after emitting the
  // signal.
  connect(this, SIGNAL(wantToAddTreeItem(QTreeWidgetItem *, QTreeWidgetItem *)),
          this, SLOT(addTreeItem(QTreeWidgetItem *, QTreeWidgetItem *)),
          Qt::BlockingQueuedConnection);
  connect(this, SIGNAL(wantToReload()), this, SLOT(reloadFilebrowser()),
          Qt::QueuedConnection);
  connect(this, SIGNAL(uploadError(int)), this, SLOT(showUploadError(int)),
          Qt::QueuedConnection);

  this->setAcceptDrops(true);

  usblink_tree = this;
}

void USBLinkTreeWidget::usblink_delete_callback(int progress, void *data) {
  // Only remove the treewidget item if the delete operation was successful
  if (progress != 100) {
    if (progress < 0 && usblink_tree)
      emit usblink_tree->uploadError(-progress);
    return;
  }

  QTreeWidgetItem *w = static_cast<QTreeWidgetItem *>(data);
  if (w->parent())
    w->parent()->takeChild(w->parent()->indexOfChild(w));
  else
    w->treeWidget()->takeTopLevelItem(w->treeWidget()->indexOfTopLevelItem(w));
}

void USBLinkTreeWidget::usblink_upload_callback(int progress, void *data) {
  USBLinkTreeWidget *that = static_cast<USBLinkTreeWidget *>(data);

  // TODO: Don't do a full refresh
  // Also refresh on error, in case of multiple transfers
  if ((progress == 100 || progress < 0) && usblink_queue_size() == 1)
    that->wantToReload(); // Reload the file explorer after uploads finished

  if (progress < 0)
    emit that->uploadError(-progress);

  emit that->uploadProgress(progress);
}

void USBLinkTreeWidget::usblink_download_callback(int progress, void *data) {
  USBLinkTreeWidget *that = static_cast<USBLinkTreeWidget *>(data);
  emit that->downloadProgress(progress);
}

void USBLinkTreeWidget::usblink_move_progress(int progress, void *user_data) {
  auto *item = reinterpret_cast<QTreeWidgetItem *>(user_data);

  if (progress == 100) // Success
    item->setData(
        2, Qt::UserRole,
        item->data(0, Qt::DisplayRole)); // Set internal name to new name
  else if (progress < 0) {               // Failure
    if (usblink_tree)
      emit usblink_tree->uploadError(-progress);
    item->setData(
        0, Qt::DisplayRole,
        item->data(2, Qt::UserRole)); // Reset display name to old name
  }
}

void USBLinkTreeWidget::reloadFilebrowser() {
  bool is_false = false;
  if (!doing_dirlist.compare_exchange_strong(is_false, true))
    usblink_queue_reset(); // The treeWidget is cleared, so references to items
                           // get invalid -> Get rid of them!

  this->clear();
  usblink_queue_dirlist("/", usblink_dirlist_callback, this);
}

void USBLinkTreeWidget::customContextMenuRequested(QPoint pos) {
  QMenu *menu = new QMenu(this);
  QAction *action_delete = new QAction(tr("Delete"), menu);

  context_menu_item = this->itemAt(pos);

  if (context_menu_item == nullptr ||
      context_menu_item->data(0, Qt::UserRole).toBool() == true) {
    // Is a directory
    QWidgetAction *action_new_folder = new QWidgetAction(menu);
    QLineEdit *line_new_folder = new QLineEdit(nullptr);
    line_new_folder->setPlaceholderText(tr("New folder"));
    action_new_folder->setDefaultWidget(line_new_folder);

    connect(line_new_folder, SIGNAL(returnPressed()), this, SLOT(newFolder()));
    // FIXME: Can this delete line_new_folder while in use by newFolder?
    connect(line_new_folder, SIGNAL(returnPressed()), menu, SLOT(close()));

    menu->addAction(action_new_folder);

    if (context_menu_item == nullptr || context_menu_item->childCount() > 0) {
      // Non-empty directory
      action_delete->setDisabled(true);
    }
  } else {
    // Is not a directory
    QAction *action_download = new QAction(tr("Download"), menu);
    connect(action_download, SIGNAL(triggered()), this, SLOT(downloadEntry()));
    menu->addAction(action_download);
  }

  connect(action_delete, SIGNAL(triggered()), this, SLOT(deleteEntry()));
  menu->addAction(action_delete);

  if (context_menu_item) {
    QAction *action_rename = new QAction(tr("Rename"), menu);
    connect(action_rename, &QAction::triggered,
            [this]() { this->editItem(context_menu_item); });
    menu->addAction(action_rename);
  }

  QAction *action_refresh = new QAction(tr("Refresh"), menu);
  connect(action_refresh, SIGNAL(triggered()), this, SLOT(reloadFilebrowser()));
  menu->addAction(action_refresh);

  QAction *action_upload = new QAction(tr("Upload files..."), menu);
  connect(action_upload, &QAction::triggered,
          [this]() { this->uploadFiles(); });
  menu->addAction(action_upload);

  menu->popup(this->viewport()->mapToGlobal(pos));
}

QString USBLinkTreeWidget::naturalSize(uint64_t bytes) {
  if (bytes < 4ul * 1024)
    return QString::number(bytes) + QStringLiteral(" B");
  else if (bytes < 4ul * 1024 * 1024)
    return QString::number(bytes / 1024) + QStringLiteral(" KiB");
  else if (bytes < 4ull * 1024 * 1024 * 1024)
    return QString::number(bytes / 1024 / 1024) + QStringLiteral(" MiB");
  else
    return tr("Too much");
}

QString USBLinkTreeWidget::usblink_path_item(QTreeWidgetItem *w) {
  if (!w)
    return QString();

  return usblink_path_item(w->parent()) + QStringLiteral("/") + w->text(0);
  // This crashes on 32-bit linux somehow
  // return QString("%0/%1").arg(path_parent).arg(path_this);
}

QStringList USBLinkTreeWidget::mimeTypes() const {
  // Accept everything here, decide based on the filename in dragEnterEvent
  return QStringList(QStringLiteral("text/uri-list"));
}

void USBLinkTreeWidget::dragEnterEvent(QDragEnterEvent *e) {
  if (e->mimeData()->hasUrls())
    e->acceptProposedAction();
}

bool USBLinkTreeWidget::dropMimeData(QTreeWidgetItem *parent, int index,
                                     const QMimeData *data,
                                     Qt::DropAction action) {
  if (!data->hasUrls())
    return false;

  (void)index;
  (void)action;

  auto parentDir = usblink_path_item(parent);
  if (parentDir.isEmpty())
    parentDir = QLatin1String("/");

  for (auto &&url : data->urls()) {
    auto local = QDir::toNativeSeparators(url.toLocalFile());
    auto remote = parentDir + QLatin1Char('/') + QFileInfo(local).fileName();
    usblink_queue_put_file(local.toStdString(), remote.toStdString(),
                           usblink_upload_callback, this);
  }

  return true;
}

bool USBLinkTreeWidget::usblink_dirlist_nested(QTreeWidgetItem *w) {
  // Find a directory (w or its children) to fill

  if (w->data(0, Qt::UserRole).value<bool>() == false) // Not a directory
    return false;

  if (w->data(1, Qt::UserRole).value<bool>() == false) // Not filled yet
  {
    std::string path_utf8 = usblink_path_item(w).toStdString();
    usblink_queue_dirlist(
        path_utf8, USBLinkTreeWidget::usblink_dirlist_callback_nested, w);
    return true;
  } else {
    for (int i = 0; i < w->childCount(); ++i)
      if (usblink_dirlist_nested(w->child(i)))
        return true;
  }

  return false;
}

QTreeWidgetItem *
USBLinkTreeWidget::itemForUSBLinkFile(struct usblink_file *file) {
  QString filename = QString::fromUtf8(file->filename);
  QTreeWidgetItem *item = new QTreeWidgetItem(
      {filename, file->is_dir ? QString() : naturalSize(file->size)});
  if (file->is_dir)
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsDropEnabled |
                   Qt::ItemIsEditable | Qt::ItemIsEnabled);
  else
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEditable |
                   Qt::ItemIsEnabled);
  item->setData(0, Qt::UserRole, QVariant(file->is_dir));
  item->setData(1, Qt::UserRole, QVariant(false));
  item->setData(2, Qt::UserRole, QVariant(filename));
  return item;
}

void USBLinkTreeWidget::usblink_dirlist_callback_nested(
    struct usblink_file *file, bool is_error, void *data) {
  QTreeWidgetItem *w = static_cast<QTreeWidgetItem *>(data);

  // End of enumeration or error
  if (!file) {
    w->setData(1, Qt::UserRole, QVariant(true)); // Dir is now filled

    if (!is_error) {
      // Find a dir to fill with entries
      for (int i = 0; i < w->treeWidget()->topLevelItemCount(); ++i)
        if (usblink_dirlist_nested(w->treeWidget()->topLevelItem(i)))
          return;
    }

    // FIXME: If a file is transferred concurrently, this may never be set to
    // false.
    if (usblink_queue_size() == 1)
      usblink_tree->doing_dirlist = false;

    return;
  }

  // Add directory entry to tree widget item (parent)
  emit usblink_tree->wantToAddTreeItem(itemForUSBLinkFile(file), w);
}

void USBLinkTreeWidget::usblink_dirlist_callback(struct usblink_file *file,
                                                 bool is_error, void *data) {
  if (is_error)
    return;

  auto *w = static_cast<USBLinkTreeWidget *>(data);

  // End of enumeration or error
  if (!file) {
    // Find a dir to fill with entries
    for (int i = 0; i < w->topLevelItemCount(); ++i)
      if (usblink_dirlist_nested(w->topLevelItem(i)))
        return;

    // FIXME: If a file is transferred concurrently, this may never be set to
    // false.
    if (usblink_queue_size() == 1)
      usblink_tree->doing_dirlist = false;

    return;
  }

  // Add directory entry to tree widget
  emit usblink_tree->wantToAddTreeItem(itemForUSBLinkFile(file), nullptr);
}

void USBLinkTreeWidget::dataChangedHandler(QTreeWidgetItem *item, int column) {
  // Only the name can be changed
  if (column != 0)
    return;

  std::string filepath = usblink_path_item(item->parent()).toStdString();

  std::string old_name = item->data(2, Qt::UserRole).toString().toStdString(),
              new_name =
                  item->data(0, Qt::DisplayRole).toString().toStdString();

  usblink_queue_move(filepath + "/" + old_name, filepath + "/" + new_name,
                     usblink_move_progress, item);
}

void USBLinkTreeWidget::downloadEntry() {
  if (!context_menu_item ||
      context_menu_item->data(0, Qt::UserRole).toBool()) // Is a directory
    return;

  QString dest = QFileDialog::getSaveFileName(
      this, tr("Chose save location"),
      context_menu_item->data(0, Qt::DisplayRole).toString(),
      tr("TNS file (*.tns)"));
  if (!dest.isEmpty())
    usblink_queue_download(usblink_path_item(context_menu_item).toStdString(),
                           dest.toStdString(), usblink_download_callback, this);
}

void USBLinkTreeWidget::deleteEntry() {
  if (!context_menu_item)
    return;

  usblink_queue_delete(usblink_path_item(context_menu_item).toStdString(),
                       context_menu_item->data(0, Qt::UserRole).toBool(),
                       usblink_delete_callback, context_menu_item);
}

void USBLinkTreeWidget::newFolder() {
  auto *line_edit = qobject_cast<QLineEdit *>(QObject::sender());

  // FIXME: Don't use usblink_upload_callback here, it may change behavior.
  usblink_queue_new_dir((usblink_path_item(context_menu_item) +
                         QStringLiteral("/") + line_edit->text())
                            .toStdString(),
                        usblink_upload_callback, this);
}

void USBLinkTreeWidget::addTreeItem(QTreeWidgetItem *item,
                                    QTreeWidgetItem *parent) {
  if (parent)
    parent->addChild(item);
  else
    this->addTopLevelItem(item);
}

void USBLinkTreeWidget::uploadFiles() {
  QString parentDir;
  if (context_menu_item && context_menu_item->data(0, Qt::UserRole).toBool()) {
    parentDir = usblink_path_item(context_menu_item);
  } else if (context_menu_item) {
    // If clicked on file, use its parent
    parentDir = usblink_path_item(context_menu_item->parent());
  } else {
    parentDir = QLatin1String("/");
  }

  if (parentDir.isEmpty())
    parentDir = QLatin1String("/");

  QStringList files = QFileDialog::getOpenFileNames(
      this, tr("Select files to upload"), QString(), tr("All Files (*)"));
  if (files.isEmpty())
    return;

  for (const QString &local : files) {
    QString remote = parentDir + QLatin1Char('/') + QFileInfo(local).fileName();
    usblink_queue_put_file(local.toStdString(), remote.toStdString(),
                           usblink_upload_callback, this);
  }
}

void USBLinkTreeWidget::showUploadError(int code) {
  QString msg = tr("The file transfer failed.");
  if (code == 0x15 || code == 0x0F) {
    msg += QLatin1String("\n\n") +
           tr("Error 0x%1: Invalid Path or Extension.").arg(code, 0, 16);
    msg += QLatin1String("\n") +
           tr("The calculator OS rejected the file. Possible reasons:");
    msg += QLatin1String("\n") +
           tr("- File has no extension? (Error 0x0F often means this)");
    msg += QLatin1String("\n") +
           tr("- Sending to root (/) is not allowed. Use a subfolder.");
    msg += QLatin1String("\n") +
           tr("- Sending raw .py/.lua files might be blocked. Try "
              "uploading as .tns and renaming it manually.");
  } else if (code == 1) {
    msg += QLatin1String("\n\n") +
           tr("Generic Error (Connection lost or rejected).");
  } else {
    msg += QLatin1String("\n\n") +
           tr("Error Code: 0x%1").arg(code, 2, 16, QLatin1Char('0'));
  }

  QMessageBox::critical(this, tr("Transfer Error"), msg);
}
