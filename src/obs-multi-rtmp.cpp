#include "pch.h"

#include <list>
#include <regex>
#include <filesystem>

#include "push-widget.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#define ConfigSection "obs-multi-rtmp"

static class GlobalServiceImpl : public GlobalService
{
public:
    bool RunInUIThread(std::function<void()> task) override {
        if (uiThread_ == nullptr)
            return false;
        QMetaObject::invokeMethod(uiThread_, [func = std::move(task)]() {
            func();
        });
        return true;
    }

    QThread* uiThread_ = nullptr;
    std::function<void()> saveConfig_;
} s_service;


GlobalService& GetGlobalService() {
    return s_service;
}


class MultiOutputWidget : public QDockWidget
{
    int dockLocation_;
    bool dockVisible_;
    bool reopenShown_;

public:
    MultiOutputWidget(QWidget* parent = 0)
        : QDockWidget(parent)
        , reopenShown_(false)
    {
        setWindowTitle(obs_module_text("Title"));
        setFeatures(QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetMovable);

        // save dock location
        QObject::connect(this, &QDockWidget::dockLocationChanged, [this](Qt::DockWidgetArea area) {
            dockLocation_ = area;
        });

        scroll_ = new QScrollArea(this);
        scroll_->move(0, 22);

        container_ = new QWidget(this);
        layout_ = new QGridLayout(container_);
        layout_->setAlignment(Qt::AlignmentFlag::AlignTop);

        int currow = 0;

        // init widget
        auto addButton = new QPushButton(obs_module_text("Btn.NewTarget"), container_);
        QObject::connect(addButton, &QPushButton::clicked, [this]() {
            auto pushwidget = createPushWidget(QJsonObject(), container_);
            layout_->addWidget(pushwidget);
            if (pushwidget->ShowEditDlg())
                SaveConfig();
            else
                delete pushwidget;
        });
        layout_->addWidget(addButton);

        // start all, stop all
        auto allBtnContainer = new QWidget(this);
        auto allBtnLayout = new QHBoxLayout();
        auto startAllButton = new QPushButton(obs_module_text("Btn.StartAll"), allBtnContainer);
        allBtnLayout->addWidget(startAllButton);
        auto stopAllButton = new QPushButton(obs_module_text("Btn.StopAll"), allBtnContainer);
        allBtnLayout->addWidget(stopAllButton);
        allBtnContainer->setLayout(allBtnLayout);
        layout_->addWidget(allBtnContainer);

        QObject::connect(startAllButton, &QPushButton::clicked, [this]() {
            for (auto x : GetAllPushWidgets())
                x->StartStreaming();
        });
        QObject::connect(stopAllButton, &QPushButton::clicked, [this]() {
            for (auto x : GetAllPushWidgets())
                x->StopStreaming();
        });
        
        // load config
        LoadConfig();

        scroll_->setWidgetResizable(true);
        scroll_->setWidget(container_);

        setLayout(layout_);

        resize(200, 400);
    }

    void visibleToggled(bool visible)
    {
        dockVisible_ = visible;

        if (visible == false
            && reopenShown_ == false
            && !config_has_user_value(obs_frontend_get_global_config(), ConfigSection, "DockVisible"))
        {
            reopenShown_ = true;
            QMessageBox(QMessageBox::Icon::Information, 
                obs_module_text("Notice.Title"), 
                obs_module_text("Notice.Reopen"), 
                QMessageBox::StandardButton::Ok,
                this
            ).exec();
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Resize)
        {
            scroll_->resize(width(), height() - 22);
        }
        return QDockWidget::event(event);
    }

    std::vector<PushWidget*> GetAllPushWidgets()
    {
        std::vector<PushWidget*> result;
        for(auto& c : container_->children())
        {
            if (c->objectName() == "push-widget")
            {
                auto w = dynamic_cast<PushWidget*>(c);
                result.push_back(w);
            }
        }
        return result;
    }

    void SaveConfig()
    {
        auto profile_config = obs_frontend_get_profile_config();
        
        QJsonArray targetlist;
        for(auto x : GetAllPushWidgets())
            targetlist.append(x->Config());
        QJsonObject root;
        root["targets"] = targetlist;
        QJsonDocument jsondoc;
        jsondoc.setObject(root);
        config_set_string(profile_config, ConfigSection, "json", jsondoc.toJson().toBase64().constData());

        config_save_safe(profile_config, "tmp", "bak");
    }

    void LoadConfig()
    {
        auto profile_config = obs_frontend_get_profile_config();

        QJsonObject conf;
        auto base64str = config_get_string(profile_config, ConfigSection, "json");
        if (!base64str || !*base64str) { // compatible with old version
            base64str = config_get_string(obs_frontend_get_global_config(), ConfigSection, "json");
        }

        if (base64str && *base64str)
        {
            auto bindat = QByteArray::fromBase64(base64str);
            auto jsondoc = QJsonDocument::fromJson(bindat);
            if (jsondoc.isObject()) {
                conf = jsondoc.object();

                // load succeed. remove all existing widgets
                for (auto x : GetAllPushWidgets())
                    delete x;
            }
        }

        auto targets = conf.find("targets");
        if (targets != conf.end() && targets->isArray())
        {
            for(auto x : targets->toArray())
            {
                if (x.isObject())
                {
                    auto pushwidget = createPushWidget(((QJsonValue)x).toObject(), container_);
                    layout_->addWidget(pushwidget);
                }
            }
        }
    }

private:
    QWidget* container_ = 0;
    QScrollArea* scroll_ = 0;
    QGridLayout* layout_ = 0;
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multi-rtmp", "en-US")
OBS_MODULE_AUTHOR("雷鳴 (@sorayukinoyume)")

bool obs_module_load()
{
    // check obs version
    if (obs_get_version() < MAKE_SEMANTIC_VERSION(26, 1, 0))
        return false;
    
    auto mainwin = (QMainWindow*)obs_frontend_get_main_window();
    if (mainwin == nullptr)
        return false;
    QMetaObject::invokeMethod(mainwin, []() {
        s_service.uiThread_ = QThread::currentThread();
    });

    auto dock = new MultiOutputWidget(mainwin);
    dock->setObjectName("obs-multi-rtmp-dock");
    auto action = (QAction*)obs_frontend_add_dock(dock);
    QObject::connect(action, &QAction::toggled, dock, &MultiOutputWidget::visibleToggled);

    obs_frontend_add_event_callback(
        [](enum obs_frontend_event event, void *private_data) {
            auto mainwin = static_cast<MultiOutputWidget*>(private_data);

            for(auto x: mainwin->GetAllPushWidgets())
                x->OnOBSEvent(event);

            if (event == obs_frontend_event::OBS_FRONTEND_EVENT_EXIT)
            {   
                mainwin->SaveConfig();
            }
            else if (event == obs_frontend_event::OBS_FRONTEND_EVENT_PROFILE_CHANGED)
            {
                static_cast<MultiOutputWidget*>(private_data)->LoadConfig();
            }
        }, dock
    );

    return true;
}

const char *obs_module_description(void)
{
    return "Multiple RTMP Output Plugin";
}
