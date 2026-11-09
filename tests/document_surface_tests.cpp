// SPDX-License-Identifier: GPL-3.0-only
#include "test_support.h"
#include "documentsurface.h"
#include <QQuickWidget>
#include <QQuickWindow>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWindow>

class DocumentSurfaceTests : public QObject {
  Q_OBJECT
private slots:
  void uncontainedRendererChangesWindowSurface() {
    QWidget window;
    window.show();
    QCOMPARE(window.windowHandle()->surfaceType(), QSurface::RasterSurface);
    auto *renderer = new QQuickWidget;
    renderer->hide();
    renderer->setParent(&window);
    // Control: even an unshown Quick widget changes the parent's surface.
    QCOMPARE(window.windowHandle()->surfaceType(), QSurface::OpenGLSurface);
    delete renderer;
    QCOMPARE(window.windowHandle()->surfaceType(), QSurface::OpenGLSurface);
  }
  void documentRenderersKeepGallerySurface_data() {
    QTest::addColumn<bool>("windowCreated");
    QTest::newRow("before-window-creation") << false;
    QTest::newRow("after-window-creation") << true;
  }
  void documentRenderersKeepGallerySurface() {
    QFETCH(bool, windowCreated);
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *stack = new QStackedWidget;
    layout->addWidget(stack);
    auto *covers = new QWidget;
    auto *pages = new QWidget;
    stack->addWidget(covers);
    stack->addWidget(pages);
    if (windowCreated) window.show();
    // The current and retained document each have their own native boundary.
    QVector<QWidget *> documents;
    for (int n = 0; n < 2; ++n) {
      auto *document = new QWidget;
      stack->addWidget(document);
      auto *documentLayout = new QVBoxLayout(document);
      auto *surface = new DocumentSurface(document);
      documentLayout->addWidget(surface);
      auto *renderer = new QQuickWidget;
      renderer->hide(); // Inspect surface selection without requiring a GPU.
      renderer->setParent(surface);
      documents.append(document);
    }
    window.show();
    const auto unchanged = [&] {
      QCOMPARE(window.windowHandle()->surfaceType(), QSurface::RasterSurface);
      for (auto *widget : {static_cast<QWidget *>(stack), covers, pages})
        QVERIFY(!widget->testAttribute(Qt::WA_NativeWindow));
    };
    unchanged();
    for (auto *document : documents) {
      stack->setCurrentWidget(document);
      unchanged();
      stack->setCurrentWidget(covers);
      unchanged();
    }
    stack->setCurrentWidget(pages);
    unchanged();
    for (auto *document : documents) {
      stack->removeWidget(document);
      delete document;
      unchanged();
    }
  }
};

int main(int argc, char **argv) {
  // A separate process selects RHI explicitly: offscreen normally substitutes
  // the Software scene graph, which cannot detect the composition regression.
  // Quick widgets remain hidden; no Chromium or working GL context is needed.
  QQuickWindow::setSceneGraphBackend("rhi");
  QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
  return isolatedTestMain<DocumentSurfaceTests>(argc, argv);
}
#include "document_surface_tests.moc"
