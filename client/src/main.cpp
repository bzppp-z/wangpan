#include <QApplication>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

// 阶段 2 Task 1：仅验证 Qt 工程可构建运行；Task 2 起替换为登录窗启动逻辑。
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QWidget window;
    window.setWindowTitle(QStringLiteral("网盘 · 客户端骨架"));
    window.resize(400, 300);

    QVBoxLayout* layout = new QVBoxLayout(&window);
    QLabel* label = new QLabel(QStringLiteral("等待登录界面"), &window);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);

    window.show();
    return app.exec();
}
