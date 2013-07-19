/*
 * This file is part of Maliit Plugins
 *
 * Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies). All rights reserved.
 *
 * Contact: Mohammad Anwari <Mohammad.Anwari@nokia.com>
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice, this list
 * of conditions and the following disclaimer in the documentation and/or other materials
 * provided with the distribution.
 * Neither the name of Nokia Corporation nor the names of its contributors may be
 * used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "inputmethod.h"
#include "editor.h"
#include "updatenotifier.h"
#include "maliitcontext.h"

#include "models/key.h"
#include "models/keyarea.h"
#include "models/wordribbon.h"
#include "models/layout.h"

#include "logic/layouthelper.h"
#include "logic/layoutupdater.h"
#include "logic/wordengine.h"
#include "logic/style.h"
#include "logic/languagefeatures.h"
#include "logic/eventhandler.h"
#include "logic/keyareaconverter.h"
#include "logic/dynamiclayout.h"

#include "view/setup.h"

#ifdef HAVE_QT_MOBILITY
#include "view/soundfeedback.h"
typedef MaliitKeyboard::SoundFeedback DefaultFeedback;
#else
#include "view/nullfeedback.h"
typedef MaliitKeyboard::NullFeedback DefaultFeedback;
#endif

#include <maliit/plugins/subviewdescription.h>
#include <maliit/plugins/abstractpluginsetting.h>
#include <maliit/plugins/updateevent.h>
#include <maliit/plugins/abstractinputmethodhost.h>


#include <QApplication>
#include <QWidget>
#include <QDesktopWidget>
#include <QtQuick>

#ifdef QT_OPENGL_ES_2
#include <ubuntu/ui/ubuntu_ui_session_service.h>
  #define HAVE_UBUNTU_PLATFORM_API
#endif

class MImUpdateEvent;

namespace MaliitKeyboard {

typedef QScopedPointer<Maliit::Plugins::AbstractPluginSetting> ScopedSetting;
typedef QSharedPointer<MKeyOverride> SharedOverride;
typedef QMap<QString, SharedOverride>::const_iterator OverridesIterator;

namespace {

const QString g_maliit_keyboard_qml(MALIIT_KEYBOARD_DATA_DIR "/maliit-keyboard.qml");
const QString g_maliit_keyboard_extended_qml(MALIIT_KEYBOARD_DATA_DIR "/maliit-keyboard-extended.qml");
const QString g_maliit_magnifier_qml(MALIIT_KEYBOARD_DATA_DIR "/maliit-magnifier.qml");

Key overrideToKey(const SharedOverride &override)
{
    Key key;

    key.rLabel().setText(override->label());
    key.setIcon(override->icon().toUtf8());
    // TODO: hightlighted and enabled information are not available in
    // Key. Should we just really create a KeyOverride model?

    return key;
}

} // unnamed namespace

class Settings
{
public:
    ScopedSetting style;
    ScopedSetting feedback;
    ScopedSetting auto_correct;
    ScopedSetting auto_caps;
    ScopedSetting word_engine;
    ScopedSetting hide_word_ribbon_in_portrait_mode;
};

class LayoutGroup
{
public:
    Logic::LayoutHelper helper;
    Logic::LayoutUpdater updater;
    Model::Layout model;
    Logic::EventHandler event_handler;

    explicit LayoutGroup();
};

LayoutGroup::LayoutGroup()
    : helper()
    , updater()
    , model()
    , event_handler(&model, &updater)
{}

QQuickView *createWindow(MAbstractInputMethodHost *host)
{
    QScopedPointer<QQuickView> view(new QQuickView);

    QSurfaceFormat format;
    format.setAlphaBufferSize(8);
    view->setFormat(format);
    view->setColor(QColor(Qt::transparent));

    host->registerWindow(view.data(), Maliit::PositionCenterBottom);

    return view.take();
}

class InputMethodPrivate
{
public:
    QQuickItem* qmlRootItem;
#ifdef EXTENDED_SURFACE_TEMP_DISABLED
    SharedSurface extended_surface;
    SharedSurface magnifier_surface;
#endif
    Editor editor;
    DefaultFeedback feedback;
    SharedStyle style;
    UpdateNotifier notifier;
    QMap<QString, SharedOverride> key_overrides;
    Settings settings;
    LayoutGroup layout;
    LayoutGroup extended_layout;
    Model::Layout magnifier_layout;
    MaliitContext context;
    QRect windowGeometryRect;
    QRect keyboardVisibleRect;
    MAbstractInputMethodHost* host;
    QQuickView* view;

    bool predictionEnabled;

    explicit InputMethodPrivate(InputMethod * const q,
                                MAbstractInputMethodHost *host);
    void setLayoutOrientation(Logic::LayoutHelper::Orientation orientation);
    void updateKeyboardOrientation();
    void syncWordEngine(Logic::LayoutHelper::Orientation orientation);

    void connectToNotifier();
    void setContextProperties(QQmlContext *qml_context);
};


InputMethodPrivate::InputMethodPrivate(InputMethod *const q,
                                       MAbstractInputMethodHost *host)
  //    : surface_factory(host->surfaceFactory())
  //    , surface(qSharedPointerDynamicCast<Surface>(surface_factory->create(g_surface_options)))
#ifdef EXTENDED_SURFACE_TEMP_DISABLED
    , extended_surface(qSharedPointerDynamicCast<Surface>(surface_factory->create(g_extended_surface_options, surface)))
    , magnifier_surface(qSharedPointerDynamicCast<Surface>(surface_factory->create(g_extended_surface_options, surface)))
#endif
    : editor(EditorOptions(), new Model::Text, new Logic::WordEngine, new Logic::LanguageFeatures)
    , feedback()
    , style(new Style)
    , notifier()
    , key_overrides()
    , settings()
    , layout()
    , extended_layout()
    , magnifier_layout()
    , context(q, style)
    , host(host)
    , view(0)
    , predictionEnabled(false)
{
    view = createWindow(host);

    editor.setHost(host);

    layout.updater.setLayout(&layout.helper);
    extended_layout.updater.setLayout(&extended_layout.helper);

    layout.updater.setStyle(style);
    extended_layout.updater.setStyle(style);
    feedback.setStyle(style);

    const QSize &screen_size(view->screen()->size());
    layout.helper.setScreenSize(screen_size);
    layout.helper.setAlignment(Logic::LayoutHelper::Bottom);
    extended_layout.helper.setScreenSize(screen_size);
    extended_layout.helper.setAlignment(Logic::LayoutHelper::Floating);

    QObject::connect(&layout.event_handler, SIGNAL(wordCandidatePressed(WordCandidate)),
                     &layout.updater, SLOT( onWordCandidatePressed(WordCandidate) ));

    QObject::connect(&layout.event_handler, SIGNAL(wordCandidateReleased(WordCandidate)),
                     &layout.updater, SLOT( onWordCandidateReleased(WordCandidate) ));

    QObject::connect(&editor,  SIGNAL(preeditEnabledChanged(bool)),
                     &layout.updater, SLOT(setWordRibbonVisible(bool)));

    QObject::connect(&layout.updater, SIGNAL(wordCandidateSelected(QString)),
                     editor.wordEngine(),  SLOT(onWordCandidateSelected(QString)));

    QObject::connect(&layout.updater, SIGNAL(languageChanged(QString)),
                     editor.wordEngine(),  SLOT(onLanguageChanged(QString)));

    QObject::connect(&layout.updater, SIGNAL(languageChanged(QString)),
                     &editor,  SLOT(onLanguageChanged(const QString&)));

    // just for now
    layout.updater.setWordRibbonVisible(true);

#ifdef EXTENDED_SURFACE_TEMP_DISABLED
    QObject::connect(&layout.event_handler,          SIGNAL(extendedKeysShown(Key)),
                     &extended_layout.event_handler, SLOT(onExtendedKeysShown(Key)));
#endif
    connectToNotifier();

#ifdef DISABLED_FLAGS_FROM_SURFACE
    view->setFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                      | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus);
#endif
    view->setWindowState(Qt::WindowNoState);

    QSurfaceFormat format;
    format.setAlphaBufferSize(8);
    view->setFormat(format);
    view->setColor(QColor(Qt::transparent));

    view->setVisible(false);

    // TODO: Figure out whether two views can share one engine.
    QQmlEngine *const engine(view->engine());
    engine->addImportPath(MALIIT_KEYBOARD_DATA_DIR);
    setContextProperties(engine->rootContext());

    view->setSource(QUrl::fromLocalFile(g_maliit_keyboard_qml));

#ifdef EXTENDED_SURFACE_TEMP_DISABLED
    QQmlEngine *const extended_engine(extended_surface->view()->engine());
    extended_engine->addImportPath(MALIIT_KEYBOARD_DATA_DIR);
    setContextProperties(extended_engine->rootContext());

    extended_surface->view()->setSource(QUrl::fromLocalFile(g_maliit_keyboard_extended_qml));

    QQmlEngine *const magnifier_engine(magnifier_surface->view()->engine());
    magnifier_engine->addImportPath(MALIIT_KEYBOARD_DATA_DIR);
    setContextProperties(magnifier_engine->rootContext());

    magnifier_surface->view()->setSource(QUrl::fromLocalFile(g_maliit_magnifier_qml));
#endif
    view->setProperty("role", 7);

    qmlRootItem = view->rootObject();

    QObject::connect(
                qmlRootItem,
                SIGNAL(hideAnimationFinishedChanged()),
                q,
                SLOT(onHideAnimationFinished()));

    // workaround: contentOrientationChanged signal not fired by app/sdk
    // http://qt-project.org/doc/qt-5.0/qtgui/qwindow.html#contentOrientation-prop
    // this is normally handled by qmallitplatforminputcontextplugin in QtBase plugins
    QObject::connect(view->screen(),
                        SIGNAL(orientationChanged(Qt::ScreenOrientation)),
                        q,
                        SLOT(deviceOrientationChanged(Qt::ScreenOrientation)));

    // workaround: resizeMode not working in current qpa imlementation
    // http://qt-project.org/doc/qt-5.0/qtquick/qquickview.html#ResizeMode-enum
    view->setResizeMode(QQuickView::SizeRootObjectToView);
}

void InputMethodPrivate::setLayoutOrientation(Logic::LayoutHelper::Orientation orientation)
{
    syncWordEngine(orientation);
    layout.updater.setOrientation(orientation);
    extended_layout.updater.setOrientation(orientation);

    windowGeometryRect = uiConst->windowGeometryRect(view->screen()->orientation());

    keyboardVisibleRect = windowGeometryRect.adjusted(0,uiConst->invisibleTouchAreaHeight(orientation),0,0);

    // qpa does not rotate the coordinate system
    windowGeometryRect = qGuiApp->primaryScreen()->mapBetween(
                    view->screen()->orientation(),
                    qGuiApp->primaryScreen()->primaryOrientation(),
                    windowGeometryRect);


    view->setGeometry(windowGeometryRect);

    if (qmlRootItem->property("shown").toBool()) {
        host->setScreenRegion(QRegion(keyboardVisibleRect));

        QRect rect(keyboardVisibleRect);
        rect.moveTop( windowGeometryRect.height() - keyboardVisibleRect.height() );
        host->setInputMethodArea(rect, view);
    }

#ifdef HAVE_UBUNTU_PLATFORM_API
    if (qmlRootItem->property("shown").toBool()) {
        ubuntu_ui_report_osk_invisible();

        qDebug() << "keyboard is reporting: total <x y w h>: <"
                 << windowGeometryRect.x()
                 << windowGeometryRect.y()
                 << windowGeometryRect.width()
                 << windowGeometryRect.height()
                 << "> and visible <"
                 << keyboardVisibleRect.x()
                 << keyboardVisibleRect.y()
                 << keyboardVisibleRect.width()
                 << keyboardVisibleRect.height()
                 << "> to the app manager.";

        // report the visible part as input trap, the invisible part can click through, e.g. browser url bar
        ubuntu_ui_report_osk_visible(
                    keyboardVisibleRect.x(),
                    keyboardVisibleRect.y(),
                    keyboardVisibleRect.width(),
                    keyboardVisibleRect.height()
                    );
    }

#endif
}

void InputMethodPrivate::updateKeyboardOrientation()
{
    setLayoutOrientation(uiConst->screenToMaliitOrientation(QGuiApplication::primaryScreen()->orientation()));
}

void InputMethodPrivate::syncWordEngine(Logic::LayoutHelper::Orientation orientation)
{
    // hide_word_ribbon_in_potrait_mode_setting overrides word_engine_setting:
#ifndef DISABLE_PREEDIT
    const bool override_activation(settings.hide_word_ribbon_in_portrait_mode->value().toBool()
                                   && orientation == Logic::LayoutHelper::Portrait);
#else
    Q_UNUSED(orientation)
    const bool override_activation = true;
#endif


    editor.wordEngine()->setEnabled(override_activation
                                    ? false
                                    : settings.word_engine->value().toBool());
}

void InputMethodPrivate::connectToNotifier()
{
#ifdef TEMP_DISABLED
    QObject::connect(&notifier, SIGNAL(cursorPositionChanged(int, QString)),
                     &editor,   SLOT(onCursorPositionChanged(int, QString)));
#endif
    QObject::connect(&notifier,      SIGNAL(keysOverriden(Logic::KeyOverrides, bool)),
                     &layout.helper, SLOT(onKeysOverriden(Logic::KeyOverrides, bool)));
}

void InputMethodPrivate::setContextProperties(QQmlContext *qml_context)
{
    qml_context->setContextProperty("maliit", &context);
    qml_context->setContextProperty("maliit_layout", &layout.model);
    qml_context->setContextProperty("maliit_event_handler", &layout.event_handler);
    qml_context->setContextProperty("maliit_extended_layout", &extended_layout.model);
    qml_context->setContextProperty("maliit_extended_event_handler", &extended_layout.event_handler);
    qml_context->setContextProperty("maliit_magnifier_layout", &magnifier_layout);
    qml_context->setContextProperty("maliit_wordribbon", layout.helper.wordRibbon());
}

InputMethod::InputMethod(MAbstractInputMethodHost *host)
    : MAbstractInputMethod(host)
    , d_ptr(new InputMethodPrivate(this, host))
{
    Q_D(InputMethod);

    // FIXME: Reconnect feedback instance.
    Setup::connectAll(&d->layout.event_handler, &d->layout.updater, &d->editor);
    Setup::connectAll(&d->extended_layout.event_handler, &d->extended_layout.updater, &d->editor);

    connect(&d->layout.helper, SIGNAL(centerPanelChanged(KeyArea,Logic::KeyOverrides)),
            &d->layout.model, SLOT(setKeyArea(KeyArea)));

    connect(&d->extended_layout.helper, SIGNAL(extendedPanelChanged(KeyArea,Logic::KeyOverrides)),
            &d->extended_layout.model, SLOT(setKeyArea(KeyArea)));

    connect(&d->layout.helper,    SIGNAL(magnifierChanged(KeyArea)),
            &d->magnifier_layout, SLOT(setKeyArea(KeyArea)));

#ifdef EXTENDED_SURFACE_TEMP_DISABLED
    connect(&d->layout.model, SIGNAL(widthChanged(int)),
            this,             SLOT(onLayoutWidthChanged(int)));

    connect(&d->layout.model, SIGNAL(heightChanged(int)),
            this,             SLOT(onLayoutHeightChanged(int)));

    connect(&d->layout.updater, SIGNAL(keyboardTitleChanged(QString)),
            &d->layout.model,   SLOT(setTitle(QString)));

    connect(&d->extended_layout.model, SIGNAL(widthChanged(int)),
            this,                      SLOT(onExtendedLayoutWidthChanged(int)));

    connect(&d->extended_layout.model, SIGNAL(heightChanged(int)),
            this,                      SLOT(onExtendedLayoutHeightChanged(int)));

    connect(&d->extended_layout.model, SIGNAL(originChanged(QPoint)),
            this,                      SLOT(onExtendedLayoutOriginChanged(QPoint)));

    connect(&d->magnifier_layout, SIGNAL(widthChanged(int)),
            this,                 SLOT(onMagnifierLayoutWidthChanged(int)));

    connect(&d->magnifier_layout, SIGNAL(heightChanged(int)),
            this,                 SLOT(onMagnifierLayoutHeightChanged(int)));

    connect(&d->magnifier_layout, SIGNAL(originChanged(QPoint)),
            this,                 SLOT(onMagnifierLayoutOriginChanged(QPoint)));
#endif

    connect(&d->editor, SIGNAL(rightLayoutSelected()),
            this,       SLOT(onRightLayoutSelected()));

    connect(this, SIGNAL(wordEngineEnabledChanged(bool)), uiConst, SLOT(onWordEngineSettingsChanged(bool)));

    connect(this, SIGNAL(predictionEnabledChanged()), this, SLOT(updateWordEngine()));

    registerStyleSetting(host);

    registerFeedbackSetting(host);
    registerAutoCorrectSetting(host);
    registerAutoCapsSetting(host);
    registerWordEngineSetting(host);
    registerHideWordRibbonInPortraitModeSetting(host);

    setActiveSubView("en_us");

    // Setting layout orientation depends on word engine and hide word ribbon
    // settings to be initialized first:

    d->updateKeyboardOrientation();
}

InputMethod::~InputMethod()
{}

void InputMethod::show()
{
    Q_D(InputMethod);

    d->view->setVisible(true);
#ifdef EXTENDED_SURFACE_TEMP_DISABLED
    d->surface->show();
    d->extended_surface->show();
    d->magnifier_surface->show();
#endif
    inputMethodHost()->setScreenRegion(QRegion(d->keyboardVisibleRect));

    QRect rect(d->keyboardVisibleRect);
    rect.moveTop( d->windowGeometryRect.height() - d->keyboardVisibleRect.height() );
    inputMethodHost()->setInputMethodArea(rect, d->view);

#ifdef HAVE_UBUNTU_PLATFORM_API
    qDebug() << "keyboard is reporting <x y w h>: <"
                << d->keyboardVisibleRect.x()
                << d->keyboardVisibleRect.y()
                << d->keyboardVisibleRect.width()
                << d->keyboardVisibleRect.height()
                << "> to the app manager.";

    ubuntu_ui_report_osk_visible(
                d->keyboardVisibleRect.x(),
                d->keyboardVisibleRect.y(),
                d->keyboardVisibleRect.width(),
                d->keyboardVisibleRect.height()
                );
#endif

    d->qmlRootItem->setProperty("shown", true);
}

void InputMethod::hide()
{
    Q_D(InputMethod);
    d->layout.updater.resetOnKeyboardClosed();
    d->editor.clearPreedit();

    d->view->setVisible(false);
#ifdef EXTENDED_SURFACE_TEMP_DISABLED
    d->surface->hide();
    d->extended_surface->hide();
    d->magnifier_surface->hide();

    const QRegion r;
    inputMethodHost()->setScreenRegion(r);
    inputMethodHost()->setInputMethodArea(r);
#endif

#ifdef HAVE_UBUNTU_PLATFORM_API
    ubuntu_ui_report_osk_invisible();
#endif

    d->qmlRootItem->setProperty("shown", false);
}

void InputMethod::setPreedit(const QString &preedit,
                             int cursor_position)
{
    Q_UNUSED(cursor_position)
    Q_D(InputMethod);
    d->editor.replacePreedit(preedit);
}

void InputMethod::switchContext(Maliit::SwitchDirection direction,
                                bool animated)
{
    Q_UNUSED(direction)
    Q_UNUSED(animated)
}

QList<MAbstractInputMethod::MInputMethodSubView>
InputMethod::subViews(Maliit::HandlerState state) const
{
    Q_UNUSED(state)
    Q_D(const InputMethod);

    QList<MInputMethodSubView> views;

    Q_FOREACH (const QString &id, d->layout.updater.keyboardIds()) {
        MInputMethodSubView v;
        v.subViewId = id;
        v.subViewTitle = d->layout.updater.keyboardTitle(id);
        views.append(v);
    }

    return views;
}

void InputMethod::setActiveSubView(const QString &id,
                                   Maliit::HandlerState state)
{
    Q_UNUSED(state)
    Q_D(InputMethod);

    // FIXME: Perhaps better to let both LayoutUpdater share the same KeyboardLoader instance?
    d->layout.updater.setActiveKeyboardId(id);
    d->extended_layout.updater.setActiveKeyboardId(id);
}

QString InputMethod::activeSubView(Maliit::HandlerState state) const
{
    Q_UNUSED(state)
    Q_D(const InputMethod);

    return d->layout.updater.activeKeyboardId();
}

void InputMethod::handleFocusChange(bool focusIn) {
    if (not focusIn)
        hide();
}

void InputMethod::handleAppOrientationChanged(int angle)
{
    Q_UNUSED(angle);

#ifdef DISABLED_AS_CONTENT_ORIENTATION_NOT_WORKING
    Q_D(InputMethod);
    d->updateKeyboardOrientation();
#endif
}

bool InputMethod::imExtensionEvent(MImExtensionEvent *event)
{
    Q_D(InputMethod);

    if (not event or event->type() != MImExtensionEvent::Update) {
        return false;
    }

    MImUpdateEvent *update_event(static_cast<MImUpdateEvent *>(event));

    d->notifier.notify(update_event);

    return true;
}


void InputMethod::registerStyleSetting(MAbstractInputMethodHost *host)
{
    Q_D(InputMethod);

    QVariantMap attributes;
    QStringList available_styles = d->style->availableProfiles();
    attributes[Maliit::SettingEntryAttributes::defaultValue] = MALIIT_DEFAULT_PROFILE;
    attributes[Maliit::SettingEntryAttributes::valueDomain] = available_styles;
    attributes[Maliit::SettingEntryAttributes::valueDomainDescriptions] = available_styles;

    d->settings.style.reset(host->registerPluginSetting("current_style",
                                                        QT_TR_NOOP("Keyboard style"),
                                                        Maliit::StringType,
                                                        attributes));

    connect(d->settings.style.data(), SIGNAL(valueChanged()),
            this,                     SLOT(onStyleSettingChanged()));

    // Call manually for the first time to initialize dependent values:
    onStyleSettingChanged();
}

void InputMethod::registerFeedbackSetting(MAbstractInputMethodHost *host)
{
    Q_D(InputMethod);

    QVariantMap attributes;
    attributes[Maliit::SettingEntryAttributes::defaultValue] = true;

    d->settings.feedback.reset(host->registerPluginSetting("feedback_enabled",
                                                           QT_TR_NOOP("Feedback enabled"),
                                                           Maliit::BoolType,
                                                           attributes));

    connect(d->settings.feedback.data(), SIGNAL(valueChanged()),
            this,                        SLOT(onFeedbackSettingChanged()));

    d->feedback.setEnabled(d->settings.feedback->value().toBool());
}

void InputMethod::registerAutoCorrectSetting(MAbstractInputMethodHost *host)
{
    Q_D(InputMethod);

    QVariantMap attributes;
    attributes[Maliit::SettingEntryAttributes::defaultValue] = true;

    d->settings.auto_correct.reset(host->registerPluginSetting("auto_correct_enabled",
                                                               QT_TR_NOOP("Auto-correct enabled"),
                                                               Maliit::BoolType,
                                                               attributes));

    connect(d->settings.auto_correct.data(), SIGNAL(valueChanged()),
            this,                            SLOT(onAutoCorrectSettingChanged()));

    d->editor.setAutoCorrectEnabled(d->settings.auto_correct->value().toBool());
}

void InputMethod::registerAutoCapsSetting(MAbstractInputMethodHost *host)
{
    Q_D(InputMethod);

    QVariantMap attributes;
    attributes[Maliit::SettingEntryAttributes::defaultValue] = true;

    d->settings.auto_caps.reset(host->registerPluginSetting("auto_caps_enabled",
                                                            QT_TR_NOOP("Auto-capitalization enabled"),
                                                            Maliit::BoolType,
                                                            attributes));

    connect(d->settings.auto_caps.data(), SIGNAL(valueChanged()),
            this,                         SLOT(onAutoCapsSettingChanged()));

    d->editor.setAutoCapsEnabled(d->settings.auto_caps->value().toBool());
}

void InputMethod::registerWordEngineSetting(MAbstractInputMethodHost *host)
{
    Q_D(InputMethod);

    QVariantMap attributes;
    attributes[Maliit::SettingEntryAttributes::defaultValue] = false;

    d->settings.word_engine.reset(host->registerPluginSetting("word_engine_enabled",
                                                              QT_TR_NOOP("Error correction/word prediction enabled"),
                                                              Maliit::BoolType,
                                                              attributes));

    connect(d->settings.word_engine.data(), SIGNAL(valueChanged()),
            this,                           SLOT(onWordEngineSettingChanged()));

    Q_EMIT wordEngineEnabledChanged( d->settings.word_engine.data()->value().toBool() );

#ifndef DISABLE_PREEDIT
    d->layout.helper.wordRibbon()->setEnabled(d->settings.word_engine->value().toBool());
    d->editor.wordEngine()->setEnabled(d->settings.word_engine->value().toBool());
#else
    d->editor.wordEngine()->setEnabled(false);
#endif
}

void InputMethod::registerHideWordRibbonInPortraitModeSetting(MAbstractInputMethodHost *host)
{
    Q_D(InputMethod);

    QVariantMap attributes;
    attributes[Maliit::SettingEntryAttributes::defaultValue] = false;

    d->settings.hide_word_ribbon_in_portrait_mode.reset(
        host->registerPluginSetting("hide_word_ribbon_in_potrait_mode",
                                    QT_TR_NOOP("Disable word engine in portrait mode"),
                                    Maliit::BoolType,
                                    attributes));

    connect(d->settings.hide_word_ribbon_in_portrait_mode.data(), SIGNAL(valueChanged()),
            this, SLOT(onHideWordRibbonInPortraitModeSettingChanged()));
}

void InputMethod::onLeftLayoutSelected()
{
    // This API smells real bad.
    const QList<MImSubViewDescription> &list =
        inputMethodHost()->surroundingSubViewDescriptions(Maliit::OnScreen);

    if (list.count() > 0) {
        Q_EMIT activeSubViewChanged(list.at(0).id());
    }
}

void InputMethod::onRightLayoutSelected()
{
    // This API smells real bad.
    const QList<MImSubViewDescription> &list =
        inputMethodHost()->surroundingSubViewDescriptions(Maliit::OnScreen);

    if (list.count() > 1) {
        Q_EMIT activeSubViewChanged(list.at(1).id());
    }
}

void InputMethod::onScreenSizeChange(const QSize &size)
{
    Q_D(InputMethod);

    d->layout.helper.setScreenSize(size);
    d->extended_layout.helper.setScreenSize(d->layout.helper.screenSize());

    d->updateKeyboardOrientation();
}

void InputMethod::onStyleSettingChanged()
{
    Q_D(InputMethod);
    d->style->setProfile(d->settings.style->value().toString());
    d->layout.model.setImageDirectory(d->style->directory(Style::Images));
    d->extended_layout.model.setImageDirectory(d->style->directory(Style::Images));
    d->magnifier_layout.setImageDirectory(d->style->directory(Style::Images));
}

void InputMethod::onFeedbackSettingChanged()
{
    Q_D(InputMethod);
    d->feedback.setEnabled(d->settings.feedback->value().toBool());
}

void InputMethod::onAutoCorrectSettingChanged()
{
    Q_D(InputMethod);
    d->editor.setAutoCorrectEnabled(d->settings.auto_correct->value().toBool());
}

void InputMethod::onAutoCapsSettingChanged()
{
    Q_D(InputMethod);
    d->editor.setAutoCapsEnabled(d->settings.auto_caps->value().toBool());
}

void InputMethod::onWordEngineSettingChanged()
{
    // FIXME: Renderer doesn't seem to update graphics properly. Word ribbon
    // is still visible until next VKB show/hide.
    Q_D(InputMethod);
    Q_EMIT wordEngineEnabledChanged( d->settings.word_engine.data()->value().toBool() );
    d->syncWordEngine(d->layout.helper.orientation());
}

void InputMethod::onHideWordRibbonInPortraitModeSettingChanged()
{
    Q_D(InputMethod);
    d->setLayoutOrientation(d->layout.helper.orientation());
}

void InputMethod::setKeyOverrides(const QMap<QString, QSharedPointer<MKeyOverride> > &overrides)
{
    Q_D(InputMethod);

    for (OverridesIterator i(d->key_overrides.begin()), e(d->key_overrides.end()); i != e; ++i) {
        const SharedOverride &override(i.value());

        if (override) {
            disconnect(override.data(), SIGNAL(keyAttributesChanged(const QString &, const MKeyOverride::KeyOverrideAttributes)),
                       this,            SLOT(updateKey(const QString &, const MKeyOverride::KeyOverrideAttributes)));
        }
    }

    d->key_overrides.clear();
    QMap<QString, Key> overriden_keys;

    for (OverridesIterator i(overrides.begin()), e(overrides.end()); i != e; ++i) {
        const SharedOverride &override(i.value());

        if (override) {
            d->key_overrides.insert(i.key(), override);
            connect(override.data(), SIGNAL(keyAttributesChanged(const QString &, const MKeyOverride::KeyOverrideAttributes)),
                    this,            SLOT(updateKey(const QString &, const MKeyOverride::KeyOverrideAttributes)));
            overriden_keys.insert(i.key(), overrideToKey(override));
        }
    }
    d->notifier.notifyOverride(overriden_keys);
}

void InputMethod::updateKey(const QString &key_id,
                            const MKeyOverride::KeyOverrideAttributes changed_attributes)
{
    Q_D(InputMethod);

    Q_UNUSED(changed_attributes);

    QMap<QString, SharedOverride>::iterator iter(d->key_overrides.find(key_id));

    if (iter != d->key_overrides.end()) {
        const Key &override_key(overrideToKey(iter.value()));
        Logic::KeyOverrides overrides_update;

        overrides_update.insert(key_id, override_key);
        d->notifier.notifyOverride(overrides_update, true);
    }
}

void InputMethod::onKeyboardClosed()
{
    hide();
    inputMethodHost()->notifyImInitiatedHiding();
}

void InputMethod::onLayoutWidthChanged(int width)
{
  Q_UNUSED(width);
}

void InputMethod::onLayoutHeightChanged(int height)
{
  Q_UNUSED(height);
}

#ifdef EXTENDED_SURFACE_TEMP_DISABLED
void InputMethod::onExtendedLayoutWidthChanged(int width)
{
    Q_D(InputMethod);
    d->extended_surface->setSize(QSize(width, d->extended_surface->size().height()));
}

void InputMethod::onExtendedLayoutHeightChanged(int height)
{
    Q_D(InputMethod);
    d->extended_surface->setSize(QSize(d->extended_surface->size().width(), height));
}

void InputMethod::onExtendedLayoutOriginChanged(const QPoint &origin)
{
    Q_D(InputMethod);
    d->extended_surface->setRelativePosition(origin);
}

void InputMethod::onMagnifierLayoutWidthChanged(int width)
{
    Q_D(InputMethod);
    d->magnifier_surface->setSize(QSize(width, d->magnifier_surface->size().height()));
}

void InputMethod::onMagnifierLayoutHeightChanged(int height)
{
    Q_D(InputMethod);
    d->magnifier_surface->setSize(QSize(d->magnifier_surface->size().width(), height));
}

void InputMethod::onMagnifierLayoutOriginChanged(const QPoint &origin)
{
    Q_D(InputMethod);
    d->magnifier_surface->setRelativePosition(origin);
}
#endif

void InputMethod::onHideAnimationFinished()
{
    Q_D(InputMethod);

    d->qmlRootItem->setProperty("hidePropertyAnimationFinished", false);

    if (d->qmlRootItem->property("state").toByteArray() == "HIDDEN") {
        d->host->notifyImInitiatedHiding();
        hide();
    }
}

void InputMethod::deviceOrientationChanged(Qt::ScreenOrientation orientation)
{
    Q_UNUSED(orientation);
    Q_D(InputMethod);
    d->updateKeyboardOrientation();
}

void InputMethod::update()
{
    Q_D(InputMethod);

    bool valid;

    bool emitPredictionEnabled = false;
    bool newPredictionEnabled = inputMethodHost()->predictionEnabled(valid);

    if (!valid)
        newPredictionEnabled = true;

    if (newPredictionEnabled != d->predictionEnabled) {
        d->predictionEnabled = newPredictionEnabled;
        emitPredictionEnabled = true;
    }

    if (emitPredictionEnabled)
        Q_EMIT predictionEnabledChanged();
}

void InputMethod::updateWordEngine()
{
    // FIXME stub
}

bool InputMethod::predictionEnabled()
{
    Q_D(InputMethod);
    return d->predictionEnabled;
}


} // namespace MaliitKeyboard