// SPDX-License-Identifier: GPL-3.0-only
#include "appearance.h"
#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <algorithm>

QPalette appearancePalette(bool dark) {
    QPalette palette;
    const QColor window=dark?QColor("#292c31"):QColor("#f2f3f5");
    const QColor base=dark?QColor("#202328"):QColor("#ffffff");
    const QColor text=dark?QColor("#e7e9ed"):QColor("#20242a");
    const QColor button=dark?QColor("#363b43"):QColor("#e6e9ed");
    const QColor highlight=dark?QColor("#376da5"):QColor("#2867a4");
    for(auto group:{QPalette::Active,QPalette::Inactive,QPalette::Disabled}) {
        const bool disabled=group==QPalette::Disabled;
        for(auto role:{QPalette::Window,QPalette::ToolTipBase}) palette.setColor(group,role,window);
        for(auto role:{QPalette::Text,QPalette::WindowText,QPalette::ButtonText,QPalette::ToolTipText})
            palette.setColor(group,role,disabled?(dark?QColor("#8e949e"):QColor("#747a83")):text);
        palette.setColor(group,QPalette::Base,base);
        palette.setColor(group,QPalette::AlternateBase,dark?QColor("#2b3037"):QColor("#edf1f6"));
        palette.setColor(group,QPalette::Button,button);
        palette.setColor(group,QPalette::Highlight,disabled?(dark?QColor("#444b56"):QColor("#c5cbd3")):highlight);
        palette.setColor(group,QPalette::HighlightedText,disabled?text:QColor(Qt::white));
        palette.setColor(group,QPalette::PlaceholderText,dark?QColor("#a2a8b2"):QColor("#666e78"));
        palette.setColor(group,QPalette::Link,dark?QColor("#8dbdff"):QColor("#165fa6"));
        palette.setColor(group,QPalette::LinkVisited,dark?QColor("#c4a5eb"):QColor("#7145a0"));
        palette.setColor(group,QPalette::Light,dark?QColor("#515965"):QColor("#ffffff"));
        palette.setColor(group,QPalette::Midlight,dark?QColor("#424954"):QColor("#e6e9ed"));
        palette.setColor(group,QPalette::Mid,dark?QColor("#272b31"):QColor("#a5abb4"));
        palette.setColor(group,QPalette::Dark,dark?QColor("#181a1e"):QColor("#727a85"));
        palette.setColor(group,QPalette::Shadow,dark?QColor("#101216"):QColor("#515862"));
        palette.setColor(group,QPalette::BrightText,Qt::white);
    }
    return palette;
}
namespace {
class Appearance final:public QObject {
public:
    explicit Appearance(QApplication *app):QObject(app),m_app(app),m_systemPalette(app->palette()),m_systemStyle(app->style()->objectName()) {
        app->installEventFilter(this);
        connect(app->styleHints(),&QStyleHints::colorSchemeChanged,this,[this] {if(m_mode==0 && !m_applying) apply();});
    }
    void setMode(int mode) {m_mode=std::clamp(mode,0,2);apply();}
protected:
    bool eventFilter(QObject *object,QEvent *event) override {
        if(object==m_app && event->type()==QEvent::ApplicationPaletteChange && !m_applying && m_mode==0)
            m_systemPalette=m_app->palette();
        return false;
    }
private:
    void apply() {
        m_applying=true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        m_app->styleHints()->setColorScheme(m_mode==0?Qt::ColorScheme::Unknown:m_mode==2?Qt::ColorScheme::Dark:Qt::ColorScheme::Light);
#endif
        // Native Windows/macOS styles can ignore palette colors. Fusion makes
        // an explicitly selected light/dark mode consistent across platforms.
        if(m_mode!=0 && !m_fusion) {m_app->setStyle(QStyleFactory::create("Fusion"));m_fusion=true;}
        else if(m_mode==0 && m_fusion) {
            if(auto *style=QStyleFactory::create(m_systemStyle)) m_app->setStyle(style);
            m_fusion=false;
        }
        if(m_mode==0) {
            const auto scheme=m_app->styleHints()->colorScheme();
            m_app->setPalette(scheme==Qt::ColorScheme::Unknown?m_systemPalette:appearancePalette(scheme==Qt::ColorScheme::Dark));
        } else m_app->setPalette(appearancePalette(m_mode==2));
        m_app->setProperty("komamimicAppearance",m_mode);
        m_applying=false;
    }
    QApplication *m_app;
    QPalette m_systemPalette;
    QString m_systemStyle;
    int m_mode=0;
    bool m_fusion=false,m_applying=false;
};
}
void setApplicationAppearance(int mode) {
    static QPointer<Appearance> controller;
    if(!controller) controller=new Appearance(qApp);
    controller->setMode(mode);
}
