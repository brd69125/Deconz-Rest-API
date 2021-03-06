/*
 * Copyright (c) 2016 dresden elektronik ingenieurtechnik gmbh.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QtPlugin>
#include <QtCore/qmath.h>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QTextCodec>
#include <QTime>
#include <QTimer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QUrl>
#include <QCryptographicHash>
#include <queue>
#include "colorspace.h"
#include "de_web_plugin.h"
#include "de_web_plugin_private.h"
#include "de_web_widget.h"
#include "json.h"

const char *HttpStatusOk           = "200 OK"; // OK
const char *HttpStatusAccepted     = "202 Accepted"; // Accepted but not complete
const char *HttpStatusNotModified  = "304 Not Modified"; // For ETag / If-None-Match
const char *HttpStatusBadRequest   = "400 Bad Request"; // Malformed request
const char *HttpStatusUnauthorized = "401 Unauthorized"; // Unauthorized
const char *HttpStatusForbidden    = "403 Forbidden"; // Understand request but no permission
const char *HttpStatusNotFound     = "404 Not Found"; // Requested uri not found
const char *HttpStatusServiceUnavailable = "503 Service Unavailable";
const char *HttpStatusNotImplemented = "501 Not Implemented";
const char *HttpContentHtml        = "text/html; charset=utf-8";
const char *HttpContentCss         = "text/css";
const char *HttpContentJson        = "application/json; charset=utf-8";
const char *HttpContentJS          = "text/javascript";
const char *HttpContentPNG         = "image/png";
const char *HttpContentJPG         = "image/jpg";
const char *HttpContentSVG         = "image/svg+xml";

static int checkZclAttributesDelay = 750;
static int ReadAttributesLongDelay = 5000;
static int ReadAttributesLongerDelay = 60000;
static uint MaxGroupTasks = 4;

ApiRequest::ApiRequest(const QHttpRequestHeader &h, const QStringList &p, QTcpSocket *s, const QString &c) :
    hdr(h), path(p), sock(s), content(c), version(ApiVersion_1)
{
    if (hdr.hasKey("Accept"))
    {
        if (hdr.value("Accept").contains("vnd.ddel.v1"))
        {
            version = ApiVersion_1_DDEL;
        }
    }
}

/*! Returns the apikey of a request or a empty string if not available
 */
QString ApiRequest::apikey() const
{
    if (path.length() > 1)
    {
        return path.at(1);
    }

    return QString("");
}

/*! Constructor for pimpl.
    \param parent - the main plugin
 */
DeRestPluginPrivate::DeRestPluginPrivate(QObject *parent) :
    QObject(parent)
{
    databaseTimer = new QTimer(this);
    databaseTimer->setSingleShot(true);

    connect(databaseTimer, SIGNAL(timeout()),
            this, SLOT(saveDatabaseTimerFired()));

    db = 0;
    saveDatabaseItems = 0;
#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)
        sqliteDatabaseName = QStandardPaths::standardLocations(QStandardPaths::DataLocation).first();
#else
        sqliteDatabaseName = QDesktopServices::storageLocation(QDesktopServices::DataLocation);
#endif
    sqliteDatabaseName.append("/zll.db");
    idleLimit = 0;
    idleTotalCounter = IDLE_READ_LIMIT;
    idleLastActivity = 0;
    udpSock = 0;
    haEndpoint = 0;
    gwGroupSendDelay = deCONZ::appArgumentNumeric("--group-delay", GROUP_SEND_DELAY);
    supportColorModeXyForGroups = false;
    groupDeviceMembershipChecked = false;
    gwLinkButton = false;

    apsCtrl = deCONZ::ApsController::instance();
    DBG_Assert(apsCtrl != 0);

    apsCtrl->setParameter(deCONZ::ParamOtauActive, 0);

    // starttime reference counts from here
    starttimeRef.start();

    // default configuration
    gwRunFromShellScript = false;
    gwDeleteUnknownRules = (deCONZ::appArgumentNumeric("--delete-unknown-rules", 1) == 1) ? true : false;
    gwRfConnected = false; // will be detected later
    gwRfConnectedExpected = (deCONZ::appArgumentNumeric("--auto-connect", 1) == 1) ? true : false;
    gwPermitJoinDuration = 0;
    gwNetworkOpenDuration = 60;
    gwRgbwDisplay = "1";
    gwTimezone = QString::fromStdString(getTimezone());
    gwTimeFormat = "12h";
    gwZigbeeChannel = 0;
    gwName = GW_DEFAULT_NAME;
    gwUpdateVersion = GW_SW_VERSION; // will be replaced by discovery handler
    gwUpdateChannel = "stable";
    gwReportingEnabled = (deCONZ::appArgumentNumeric("--reporting", 1) == 1) ? true : false;
    gwFirmwareNeedUpdate = false;
    gwFirmwareVersion = "0x00000000"; // query later
    gwFirmwareVersionUpdate = "";

    {
        QHttpRequestHeader hdr;
        QStringList path;
        QString content;
        ApiRequest dummyReq(hdr, path, 0, content);
        dummyReq.version = ApiVersion_1_DDEL;
        configToMap(dummyReq, gwConfig);
    }
    updateEtag(gwConfigEtag);

    // set some default might be overwritten by database
    gwAnnounceInterval = ANNOUNCE_INTERVAL;
    gwAnnounceUrl = "http://dresden-light.appspot.com/discover";
    inetDiscoveryManager = 0;

    openDb();
    initDb();
    readDb();
    closeDb();

    if (gwUuid.isEmpty())
    {
        generateGatewayUuid();
    }

    // create default group
    Group group;
    group.setAddress(0);
    group.setName("All");
    groups.push_back(group);

    initUpnpDiscovery();

    connect(apsCtrl, SIGNAL(apsdeDataConfirm(const deCONZ::ApsDataConfirm&)),
            this, SLOT(apsdeDataConfirm(const deCONZ::ApsDataConfirm&)));

    connect(apsCtrl, SIGNAL(apsdeDataIndication(const deCONZ::ApsDataIndication&)),
            this, SLOT(apsdeDataIndication(const deCONZ::ApsDataIndication&)));

    connect(apsCtrl, SIGNAL(nodeEvent(deCONZ::NodeEvent)),
            this, SLOT(nodeEvent(deCONZ::NodeEvent)));

    deCONZ::GreenPowerController *gpCtrl = deCONZ::GreenPowerController::instance();

    if (gpCtrl)
    {
        bool ok =
        connect(gpCtrl, SIGNAL(gpDataIndication(deCONZ::GpDataIndication)),
                this, SLOT(gpDataIndication(deCONZ::GpDataIndication)));

        DBG_Assert(ok);
    }

    taskTimer = new QTimer(this);
    taskTimer->setSingleShot(false);
    connect(taskTimer, SIGNAL(timeout()),
            this, SLOT(processTasks()));
    taskTimer->start(100);

    groupTaskTimer = new QTimer(this);
    groupTaskTimer->setSingleShot(false);
    connect(groupTaskTimer, SIGNAL(timeout()),
            this, SLOT(processGroupTasks()));
    groupTaskTimer->start(250);

    verifyRulesTimer = new QTimer(this);
    verifyRulesTimer->setSingleShot(false);
    verifyRulesTimer->setInterval(5000);
    connect(verifyRulesTimer, SIGNAL(timeout()),
            this, SLOT(verifyRuleBindingsTimerFired()));
    verifyRulesTimer->start();

    bindingTimer = new QTimer(this);
    bindingTimer->setSingleShot(true);
    bindingTimer->setInterval(1000);
    connect(bindingTimer, SIGNAL(timeout()),
            this, SLOT(bindingTimerFired()));

    bindingTableReaderTimer = new QTimer(this);
    bindingTableReaderTimer->setSingleShot(true);
    bindingTableReaderTimer->setInterval(1000);
    connect(bindingTableReaderTimer, SIGNAL(timeout()),
            this, SLOT(bindingTableReaderTimerFired()));

    bindingToRuleTimer = new QTimer(this);
    bindingToRuleTimer->setSingleShot(true);
    bindingToRuleTimer->setInterval(50);
    connect(bindingToRuleTimer, SIGNAL(timeout()),
            this, SLOT(bindingToRuleTimerFired()));

    lockGatewayTimer = new QTimer(this);
    lockGatewayTimer->setSingleShot(true);
    connect(lockGatewayTimer, SIGNAL(timeout()),
            this, SLOT(lockGatewayTimerFired()));

    openClientTimer = new QTimer(this);
    openClientTimer->setSingleShot(false);
    connect(openClientTimer, SIGNAL(timeout()),
            this, SLOT(openClientTimerFired()));
    openClientTimer->start(1000);

    saveCurrentRuleInDbTimer = new QTimer(this);
    saveCurrentRuleInDbTimer->setSingleShot(true);
    connect(saveCurrentRuleInDbTimer, SIGNAL(timeout()),
            this, SLOT(saveCurrentRuleInDbTimerFired()));

    initAuthentification();
    initInternetDicovery();
    initSchedules();
    initPermitJoin();
    initOtau();
    initTouchlinkApi();
    initChangeChannelApi();
    initResetDeviceApi();
    initFirmwareUpdate();
}

/*! Deconstructor for pimpl.
 */
DeRestPluginPrivate::~DeRestPluginPrivate()
{
    if (inetDiscoveryManager)
    {
        inetDiscoveryManager->deleteLater();
        inetDiscoveryManager = 0;
    }
}

/*! APSDE-DATA.indication callback.
    \param ind - the indication primitive
    \note Will be called from the main application for each incoming indication.
    Any filtering for nodes, profiles, clusters must be handled by this plugin.
 */
void DeRestPluginPrivate::apsdeDataIndication(const deCONZ::ApsDataIndication &ind)
{
    Q_Q(DeRestPlugin);
    if (!q->pluginActive())
    {
        return;
    }

    if ((ind.profileId() == HA_PROFILE_ID) || (ind.profileId() == ZLL_PROFILE_ID))
    {

        deCONZ::ZclFrame zclFrame;

        {
            QDataStream stream(ind.asdu());
            stream.setByteOrder(QDataStream::LittleEndian);
            zclFrame.readFromStream(stream);
        }

        TaskItem task;

        switch (ind.clusterId())
        {
        case GROUP_CLUSTER_ID:
            handleGroupClusterIndication(task, ind, zclFrame);
            break;

        case SCENE_CLUSTER_ID:
            handleSceneClusterIndication(task, ind, zclFrame);
            break;

        case OTAU_CLUSTER_ID:
            otauDataIndication(ind, zclFrame);
            break;
        case COMMISSIONING_CLUSTER_ID:
            handleCommissioningClusterIndication(task, ind, zclFrame);
            break;
        case ONOFF_CLUSTER_ID:
             handleOnOffClusterIndication(task, ind, zclFrame);
            break;

        default:
        {
            if (zclFrame.isProfileWideCommand() && zclFrame.commandId() == deCONZ::ZclReportAttributesId)
            {
                DBG_Printf(DBG_INFO, "ZCL attribute report 0x%016llX for cluster 0x%04X\n", ind.srcAddress().ext(), ind.clusterId());
            }
        }
            break;
        }
    }
    else if (ind.profileId() == ZDP_PROFILE_ID)
    {
        switch (ind.clusterId())
        {
        case ZDP_DEVICE_ANNCE_CLID:
            handleDeviceAnnceIndication(ind);
            break;

        case ZDP_MGMT_BIND_RSP_CLID:
            handleMgmtBindRspIndication(ind);
            break;

        case ZDP_BIND_RSP_CLID:
        case ZDP_UNBIND_RSP_CLID:
            handleBindAndUnbindRspIndication(ind);
            break;

        case ZDP_MGMT_LEAVE_RSP_CLID:
            handleMgmtLeaveRspIndication(ind);
            break;

        default:
            break;
        }
    }
    else if (ind.profileId() == DE_PROFILE_ID)
    {
        otauDataIndication(ind, deCONZ::ZclFrame());
    }
    else if (ind.profileId() == ATMEL_WSNDEMO_PROFILE_ID)
    {
        wsnDemoDataIndication(ind);
    }
}

/*! APSDE-DATA.confirm callback.
    \param conf - the confirm primitive
    \note Will be called from the main application for each incoming confirmation,
    even if the APSDE-DATA.request was not issued by this plugin.
 */
void DeRestPluginPrivate::apsdeDataConfirm(const deCONZ::ApsDataConfirm &conf)
{
    std::list<TaskItem>::iterator i = runningTasks.begin();
    std::list<TaskItem>::iterator end = runningTasks.end();

    for (;i != end; ++i)
    {
        TaskItem &task = *i;
        if (task.req.id() == conf.id())
        {
            if (conf.status() != deCONZ::ApsSuccessStatus)
            {
                DBG_Printf(DBG_INFO, "error APSDE-DATA.confirm: 0x%02X on task\n", conf.status());

                if (conf.status() == deCONZ::ApsNoAckStatus)
                {
                    if (task.taskType == TaskGetGroupIdentifiers)
                    {
                        Sensor *s = getSensorNodeForAddress(task.req.dstAddress().ext());
                        if (s && s->isAvailable())
                        {
                            s->setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
                            s->enableRead(READ_GROUP_IDENTIFIERS);
                            s->setLastRead(idleTotalCounter);
                        }
                    }
                }
            }

            DBG_Printf(DBG_INFO_L2, "Erase task zclSequenceNumber: %u\n", task.zclFrame.sequenceNumber());
            runningTasks.erase(i);
            processTasks();

            return;
        }
    }

    if (handleMgmtBindRspConfirm(conf))
    {
        return;
    }

    if (channelChangeApsRequestId == conf.id())
    {
        channelChangeSendConfirm(conf.status() == deCONZ::ApsSuccessStatus);
    }
    if (resetDeviceApsRequestId == conf.id())
    {
        resetDeviceSendConfirm(conf.status() == deCONZ::ApsSuccessStatus);
    }
}

/*! Process incoming green power button event.
    \param ind - the data indication
 */
void DeRestPluginPrivate::gpProcessButtonEvent(const deCONZ::GpDataIndication &ind)
{
    /*
        PTM 215Z DEMO

        A0 B0
        A1 B1

        DeviceId 0x02 (On/Off Switch)


             A0,B0 Press    0x64 Press   2 of 2
             A0,B0 Release  0x65 Release 2 of 2

        A0 0x10 Scene0      B0 0x22 Toggle
        A1 0x11 Scene1      B1 0x12 Scene2

             A1,B1 Press    0x62 Press   1 of 2
             A1,B1 Release  0x63 Release 1 of 2

     */

    Sensor *sensor = getSensorNodeForAddress(ind.gpdSrcId());

    if (!sensor || sensor->deletedState() == Sensor::StateDeleted)
    {
        return;
    }

    QString lastUpdatedOld = sensor->state().lastupdated();

    sensor->state().setButtonevent(ind.gpdCommandId());
    sensor->state().updateTime();
    updateEtag(sensor->etag);

    QString address = "";
    QString id = "";
    QString event = "";
    QString op = "";
    QString val = "";
    QRegExp numbers("\\d+");

    // search rules for rule that meets condition
    std::vector<Rule>::const_iterator r = rules.begin();
    std::vector<Rule>::const_iterator rEnd = rules.end();
    for (; r != rEnd; ++r)
    {
        if (r->state() != Rule::StateDeleted)
        {
            bool ok = false;
            bool ok2 = false;

            std::vector<RuleCondition>::const_iterator c = r->conditions().begin();
            std::vector<RuleCondition>::const_iterator cEnd = r->conditions().end();
            for (; c != cEnd; ++c)
            {
                address = c->address();

                int pos = numbers.indexIn(address);
                if (pos > -1)
                {
                    id = numbers.cap(0);
                }
                event = (address.indexOf("buttonevent") != -1) ? "buttonevent" : "lastupdated";
                op = c->ooperator();
                val = c->value();

                //each condition in rule must meet condition in sensor event
                if ((id != "") && (id == sensor->id()))
                {
                    if (event == "buttonevent")
                    {
                        if (val.toInt() == sensor->state().buttonevent())
                        {
                            ok = true;
                        }
                        else
                        {
                            ok = false;
                        }
                    }
                    if (event == "lastupdated")
                    {
                        if (lastUpdatedOld != sensor->state().lastupdated())
                        {
                            ok2 = true;
                        }
                        else
                        {
                            ok2 = false;
                        }
                    }
                }
            }

            QString body = "";
            QString method = "";
            QStringList idList;
            QString groupId = "";
            QString lightId = "";
            QString sceneId = "";

            // all conditions checked; if ok == true then do action
            if (ok && ok2)
            {
                saveCurrentRuleInDbTimer->stop();
                saveCurrentRuleInDbTimer->start(3000);
                std::vector<RuleAction>::const_iterator a = r->actions().begin();
                std::vector<RuleAction>::const_iterator aEnd = r->actions().end();
                for (; a != aEnd; ++a)
                {
                    Group *group = 0;
                    TaskItem task;
                    task.req.setDstEndpoint(0xFF); // broadcast endpoint
                    task.req.setSrcEndpoint(getSrcEndpoint(0, task.req));

                    address = a->address();
                    body = a->body();
                    method = a->method();

                    if (address.indexOf("scenes") != -1)
                    {
                        //recall scene
                        idList = address.split("/");
                        if (idList.size() < 5)
                        {
                            continue;
                        }
                        groupId = idList[2];
                        sceneId = idList[4];

                        group = getGroupForId(groupId);
                        if (group && group->state() != Group::StateDeleted && group->state() != Group::StateDeleteFromDB)
                        {
                            task.req.setDstAddressMode(deCONZ::ApsGroupAddress);
                            task.req.dstAddress().setGroup(group->address());
                            if (!callScene(group, sceneId.toInt()))
                            {
                                DBG_Printf(DBG_INFO, "failed to call scene\n");
                            }
                            else
                            {
                                Scene scene;
                                TaskItem task2;
                                bool colorloopDeactivated = false;

                                std::vector<Scene>::const_iterator i = group->scenes.begin();
                                std::vector<Scene>::const_iterator end = group->scenes.end();

                                for (; i != end; ++i)
                                {
                                    if ((i->id == sceneId.toInt()) && (i->state != Scene::StateDeleted))
                                    {
                                        scene = *i;

                                        std::vector<LightState>::const_iterator ls = scene.lights().begin();
                                        std::vector<LightState>::const_iterator lsend = scene.lights().end();

                                        for (; ls != lsend; ++ls)
                                        {
                                            LightNode *light = getLightNodeForId(ls->lid());
                                            if (light && light->isAvailable() && light->state() != LightNode::StateDeleted)
                                            {
                                                bool changed = false;
                                                if (!ls->colorloopActive() && light->isColorLoopActive() != ls->colorloopActive())
                                                {
                                                    //stop colorloop if scene was saved without colorloop (Osram don't stop colorloop if another scene is called)
                                                    task2.lightNode = light;
                                                    task2.req.dstAddress() = task2.lightNode->address();
                                                    task2.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
                                                    task2.req.setDstEndpoint(task2.lightNode->haEndpoint().endpoint());
                                                    task2.req.setSrcEndpoint(getSrcEndpoint(task2.lightNode, task2.req));
                                                    task2.req.setDstAddressMode(deCONZ::ApsExtAddress);

                                                    light->setColorLoopActive(false);
                                                    addTaskSetColorLoop(task2, false, 15);

                                                    changed = true;
                                                    colorloopDeactivated = true;
                                                }
                                                //turn on colorloop if scene was saved with colorloop (FLS don't save colorloop at device)
                                                else if (ls->colorloopActive() && light->isColorLoopActive() != ls->colorloopActive())
                                                {
                                                    task2.lightNode = light;
                                                    task2.req.dstAddress() = task2.lightNode->address();
                                                    task2.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
                                                    task2.req.setDstEndpoint(task2.lightNode->haEndpoint().endpoint());
                                                    task2.req.setSrcEndpoint(getSrcEndpoint(task2.lightNode, task2.req));
                                                    task2.req.setDstAddressMode(deCONZ::ApsExtAddress);

                                                    light->setColorLoopActive(true);
                                                    light->setColorLoopSpeed(ls->colorloopTime());
                                                    addTaskSetColorLoop(task2, true, ls->colorloopTime());
                                                    changed = true;
                                                }
                                                if (ls->on() && !light->isOn())
                                                {
                                                    light->setIsOn(true);
                                                    changed = true;
                                                }
                                                if (!ls->on() && light->isOn())
                                                {
                                                    light->setIsOn(false);
                                                    changed = true;
                                                }
                                                if ((uint16_t)ls->bri() != light->level())
                                                {
                                                    light->setLevel((uint16_t)ls->bri());
                                                    changed = true;
                                                }
                                                if (changed)
                                                {
                                                    updateEtag(light->etag);
                                                }
                                            }
                                        }

                                        //recall scene again
                                        if (colorloopDeactivated)
                                        {
                                            callScene(group, sceneId.toInt());
                                        }
                                        break;
                                    }
                                }
                                // turning 'on' the group is also a assumtion but a very likely one
                                if (!group->isOn())
                                {
                                    group->setIsOn(true);
                                    updateEtag(group->etag);
                                }

                                updateEtag(gwConfigEtag);

                                processTasks();
                            }
                        }
                    }
                    else if (address.indexOf("lights") != -1)
                    {
                        //change light state
                        idList = address.split("/");
                        if (idList.size() < 3)
                        {
                            continue;
                        }
                        lightId = idList[2];
                        // TODO implement
                    }
                    else if (address.indexOf("groups") != -1)
                    {
                        //do group action
                        idList = address.split("/");
                        if (idList.size() < 3)
                        {
                            continue;
                        }
                        groupId = idList[2];

                        if (groupId != "0")
                        {
                            group = getGroupForId(groupId);
                            if (!group)
                            {
                                continue;
                            }
                            task.req.setDstAddressMode(deCONZ::ApsGroupAddress);
                            task.req.dstAddress().setGroup(group->address());
                        }
                        else
                        {
                            task.req.setDstAddressMode(deCONZ::ApsNwkAddress);
                            task.req.dstAddress().setNwk(deCONZ::BroadcastRouters);
                        }
                        task.req.setState(deCONZ::FireAndForgetState);

                        if ((body.indexOf("on") != -1) && (body.indexOf("false") != -1))
                        {
                            if (!addTaskSetOnOff(task, ONOFF_COMMAND_OFF, 0))
                            {
                                DBG_Printf(DBG_INFO, "failed to send off command\n");
                            }
                            else
                            {
                                if (groupId != "0")
                                {
                                    group->setIsOn(false);
                                    updateEtag(group->etag);
                                }

                                std::vector<LightNode>::iterator l = nodes.begin();
                                std::vector<LightNode>::iterator lend = nodes.end();

                                for (; l != lend; ++l)
                                {
                                    if (groupId == "0" || (group && isLightNodeInGroup(&(*l), group->address())))
                                    {
                                        l->setIsOn(false);
                                        updateEtag(l->etag);
                                    }
                                }

                            }
                        }
                        else if ((body.indexOf("on") != -1) && (body.indexOf("true") != -1))
                        {
                            if (!addTaskSetOnOff(task, ONOFF_COMMAND_ON, 0))
                            {
                                DBG_Printf(DBG_INFO, "failed to send on command\n");
                            }
                            else
                            {
                                if (groupId != "0")
                                {
                                    group->setIsOn(true);
                                    if (group->isColorLoopActive())
                                    {
                                        TaskItem task1;
                                        task1.req.dstAddress().setGroup(group->address());
                                        task1.req.setDstAddressMode(deCONZ::ApsGroupAddress);
                                        task1.req.setDstEndpoint(0xFF); // broadcast endpoint
                                        task1.req.setSrcEndpoint(getSrcEndpoint(0, task1.req));

                                        addTaskSetColorLoop(task1, false, 15);
                                        group->setColorLoopActive(false);
                                    }
                                    updateEtag(group->etag);
                                }

                                // check each light if colorloop needs to be disabled
                                std::vector<LightNode>::iterator l = nodes.begin();
                                std::vector<LightNode>::iterator lend = nodes.end();

                                for (; l != lend; ++l)
                                {
                                    if (groupId == "0" || isLightNodeInGroup(&(*l),group->address()))
                                    {
                                        l->setIsOn(true);

                                        if (l->isAvailable() && l->state() != LightNode::StateDeleted && l->isColorLoopActive())
                                        {
                                            TaskItem task2;
                                            task2.lightNode = &(*l);
                                            task2.req.dstAddress() = task2.lightNode->address();
                                            task2.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
                                            task2.req.setDstEndpoint(task2.lightNode->haEndpoint().endpoint());
                                            task2.req.setSrcEndpoint(getSrcEndpoint(task2.lightNode, task2.req));
                                            task2.req.setDstAddressMode(deCONZ::ApsExtAddress);

                                            addTaskSetColorLoop(task2, false, 15);
                                            l->setColorLoopActive(false);
                                        }
                                        updateEtag(l->etag);
                                    }
                                }
                            }
                        }
                        updateEtag(gwConfigEtag);
                    }

                /*
                    if (ind.gpdCommandId() >= deCONZ::GpCommandIdScene0 &&
                        ind.gpdCommandId() <= deCONZ::GpCommandIdScene15)
                    {
                        // check if scene exists
                        bool ok;
                        quint8 sceneId;
                        std::vector<Scene>::const_iterator i = group->scenes.begin();
                        std::vector<Scene>::const_iterator end = group->scenes.end();

                        ok = false;
                        int count = deCONZ::GpCommandIdScene0; // 0x10

                        for (; i != end; ++i)
                        {
                            if (i->state != Scene::StateDeleted)
                            {
                                if (ind.gpdCommandId() == count)
                                {
                                    sceneId = i->id;
                                    ok = true;
                                    break;
                                }

                                count++;
                            }
                        }

                        if (ok)
                        {
                            if (!callScene(group, sceneId))
                            {
                                DBG_Printf(DBG_INFO, "failed to call scene\n");
                            }
                        }
                    }
                    else if (ind.gpdCommandId() == deCONZ::GpCommandIdToggle)
                    {
                        if (!addTaskSetOnOff(task, ONOFF_COMMAND_TOGGLE))
                        {
                            DBG_Printf(DBG_INFO, "failed to send toggle command\n");
                        }
                    }
                    else if (ind.gpdCommandId() == deCONZ::GpCommandIdPress2Of2)
                    {
                        bool withOnOff = false;
                        bool upDirection = true;
                        quint8 rate = 20;
                        if (!addTaskMoveLevel(task, withOnOff, upDirection, rate))
                        {
                            DBG_Printf(DBG_INFO, "failed to move level up\n");
                        }
                    }
                    else if (ind.gpdCommandId() == deCONZ::GpCommandIdRelease2Of2)
                    {
                        bool withOnOff = false;
                        bool upDirection = true;
                        quint8 rate = 0;
                        if (!addTaskMoveLevel(task, withOnOff, upDirection, rate))
                        {
                            DBG_Printf(DBG_INFO, "failed to stop move level\n");
                        }
                    }
                    else if (ind.gpdCommandId() == deCONZ::GpCommandIdPress1Of2)
                    {
                        bool withOnOff = false;
                        bool upDirection = false;
                        quint8 rate = 20;
                        if (!addTaskMoveLevel(task, withOnOff, upDirection, rate))
                        {
                            DBG_Printf(DBG_INFO, "failed to move level down\n");
                        }
                    }
                    else if (ind.gpdCommandId() == deCONZ::GpCommandIdRelease1Of2)
                    {
                        bool withOnOff = false;
                        bool upDirection = false;
                        quint8 rate = 0;
                        if (!addTaskMoveLevel(task, withOnOff, upDirection, rate))
                        {
                            DBG_Printf(DBG_INFO, "failed to stop move level\n");
                        }
                    }
                */
                }

                Rule *saveRule = getRuleForId(r->id());
                if (saveRule)
                {
                    saveRule->setLastTriggered(QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss"));
                    saveRule->setTimesTriggered(r->timesTriggered()+1);
                }
            }
        }
    }
}

/*! Returns the number of tasks for a specific address.
    \param address - the destination address
 */
int DeRestPluginPrivate::taskCountForAddress(const deCONZ::Address &address)
{
    int count = 0;

    {
        std::list<TaskItem>::const_iterator i = tasks.begin();
        std::list<TaskItem>::const_iterator end = tasks.end();

        for (; i != end; ++i)
        {
            if (i->req.dstAddress() == address)
            {
                count++;
            }

        }
    }

    {
        std::list<TaskItem>::const_iterator i = runningTasks.begin();
        std::list<TaskItem>::const_iterator end = runningTasks.end();

        for (; i != end; ++i)
        {
            if (i->req.dstAddress() == address)
            {
                count++;
            }

        }
    }

    return count;
}

/*! Process incoming green power data frame.
    \param ind - the data indication
 */
void DeRestPluginPrivate::gpDataIndication(const deCONZ::GpDataIndication &ind)
{
    switch (ind.gpdCommandId())
    {
    case deCONZ::GpCommandIdScene0:
    case deCONZ::GpCommandIdScene1:
    case deCONZ::GpCommandIdScene2:
    case deCONZ::GpCommandIdScene3:
    case deCONZ::GpCommandIdScene4:
    case deCONZ::GpCommandIdScene5:
    case deCONZ::GpCommandIdScene6:
    case deCONZ::GpCommandIdScene7:
    case deCONZ::GpCommandIdScene8:
    case deCONZ::GpCommandIdScene9:
    case deCONZ::GpCommandIdScene10:
    case deCONZ::GpCommandIdScene11:
    case deCONZ::GpCommandIdScene12:
    case deCONZ::GpCommandIdScene13:
    case deCONZ::GpCommandIdScene14:
    case deCONZ::GpCommandIdScene15:
    case deCONZ::GpCommandIdOn:
    case deCONZ::GpCommandIdOff:
    case deCONZ::GpCommandIdToggle:
    case deCONZ::GpCommandIdRelease:
    case deCONZ::GpCommandIdPress1Of1:
    case deCONZ::GpCommandIdRelease1Of1:
    case deCONZ::GpCommandIdPress1Of2:
    case deCONZ::GpCommandIdRelease1Of2:
    case deCONZ::GpCommandIdPress2Of2:
    case deCONZ::GpCommandIdRelease2Of2:
    {
        gpProcessButtonEvent(ind);
    }
        break;

    case deCONZ::GpCommandIdCommissioning:
    {
        // 1    8-bit enum    GPD DeviceID
        // 1    8-bit bmp     Options
        // 0/1  8-bit bmp     Extended Options
        // 0/16 Security Key  GPD Key
        // 0/4  u32           GPD Key MIC
        // 0/4  u32           GPD outgoing counter

        quint8 gpdDeviceId;
        quint8 gpdKey[16];
        quint32 gpdMIC = 0;
        quint32 gpdOutgoingCounter = 0;
        deCONZ::GPCommissioningOptions options;
        deCONZ::GpExtCommissioningOptions extOptions;
        options.byte = 0;
        extOptions.byte = 0;

        QDataStream stream(ind.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        if (stream.atEnd()) { return; }
        stream >> gpdDeviceId;

        if (stream.atEnd()) { return; }
        stream >> options.byte;

        if (options.bits.extOptionsField)
        {
            if (stream.atEnd()) { return; }
            stream >> extOptions.byte;
        }

        if (extOptions.bits.gpdKeyPresent)
        {
            for (int i = 0; i < 16; i++)
            {
                if (stream.atEnd()) { return; }
                stream >> gpdKey[i];

            }

            if (extOptions.bits.gpdKeyEncryption)
            {
                // TODO decrypt key

                if (stream.atEnd()) { return; }
                stream >> gpdMIC;
            }
        }

        switch (extOptions.bits.securityLevelCapabilities)
        {
        case 0:
        default:
            break;
        }

        if (extOptions.bits.gpdOutgoingCounterPresent)
        {
            if (stream.atEnd()) { return; }
            stream >> gpdOutgoingCounter;
        }


        SensorFingerprint fp;
        fp.endpoint = GREEN_POWER_ENDPOINT;
        fp.deviceId = gpdDeviceId;
        fp.profileId = GP_PROFILE_ID;
        fp.outClusters.push_back(GREEN_POWER_CLUSTER_ID);

        Sensor *sensor = getSensorNodeForFingerPrint(ind.gpdSrcId(), fp, "ZGPSwitch");

        if (!sensor)
        {
            // create new sensor
            Sensor sensorNode;

            if (gpdDeviceId == deCONZ::GpDeviceIdOnOffSwitch)
            {
                sensorNode.setType("ZGPSwitch");
                sensorNode.setModelId("ZGPSWITCH");
                sensorNode.setManufacturer("Philips");
                sensorNode.setSwVersion("1.0");
            }
            else
            {
                DBG_Printf(DBG_INFO, "unsupported green power device 0x%02X\n", gpdDeviceId);
                return;
            }

            sensorNode.address().setExt(ind.gpdSrcId());
            sensorNode.setUniqueId(sensorNode.address().toStringExt());
            sensorNode.fingerPrint() = fp;

            SensorConfig sensorConfig;
            sensorConfig.setReachable(true);
            sensorNode.setConfig(sensorConfig);

            openDb();
            loadSensorNodeFromDb(&sensorNode);
            closeDb();

            if (sensorNode.id().isEmpty())
            {
                openDb();
                sensorNode.setId(QString::number(getFreeSensorId()));
                closeDb();
            }

            if (sensorNode.name().isEmpty())
            {
                sensorNode.setName(QString("%1 %2").arg(sensorNode.type()).arg(sensorNode.id()));
            }

            DBG_Printf(DBG_INFO, "SensorNode %u: %s added\n", sensorNode.id().toUInt(), qPrintable(sensorNode.name()));
            updateEtag(sensorNode.etag);
            updateEtag(gwConfigEtag);

            sensors.push_back(sensorNode);
            queSaveDb(DB_SENSORS , DB_SHORT_SAVE_DELAY);
        }
        else if (sensor && sensor->deletedState() == Sensor::StateDeleted)
        {
            sensor->setDeletedState(Sensor::StateNormal);
            DBG_Printf(DBG_INFO, "SensorNode %u: %s reactivated\n", sensor->id().toUInt(), qPrintable(sensor->name()));
            updateEtag(sensor->etag);
            updateEtag(gwConfigEtag);
            queSaveDb(DB_SENSORS , DB_SHORT_SAVE_DELAY);
        }
        else
        {
            DBG_Printf(DBG_INFO, "SensorNode %s already known\n", qPrintable(sensor->name()));
        }
    }
        break;

    default:
        break;
    }
}

/*! Returns true if the ZigBee network is connected.
 */
bool DeRestPluginPrivate::isInNetwork()
{
    if (apsCtrl)
    {
        return (apsCtrl->networkState() == deCONZ::InNetwork);
    }
    return false;
}

/*! Creates a error map used in JSON response.
    \param id - error id
    \param ressource example: "/lights/2"
    \param description example: "resource, /lights/2, not available"
    \return the map
 */
QVariantMap DeRestPluginPrivate::errorToMap(int id, const QString &ressource, const QString &description)
{
    QVariantMap map;
    QVariantMap error;
    error["type"] = (double)id;
    error["address"] = ressource;
    error["description"] = description;
    map["error"] = error;

    DBG_Printf(DBG_INFO, "API error %d, %s, %s\n", id, qPrintable(ressource), qPrintable(description));

    return map;
}

/*! Creates a new unique ETag for a resource.
 */
void DeRestPluginPrivate::updateEtag(QString &etag)
{
    QTime time = QTime::currentTime();
#if QT_VERSION < 0x050000
    etag = QString(QCryptographicHash::hash(time.toString().toAscii(), QCryptographicHash::Md5).toHex());
#else
    etag = QString(QCryptographicHash::hash(time.toString().toLatin1(), QCryptographicHash::Md5).toHex());
#endif
    // quotes are mandatory as described in w3 spec
    etag.prepend('"');
    etag.append('"');
}

/*! Returns the system uptime in seconds.
 */
qint64 DeRestPluginPrivate::getUptime()
{
    DBG_Assert(starttimeRef.isValid());

    if (!starttimeRef.isValid())
    {
        starttimeRef.start();
    }

    if (starttimeRef.isValid())
    {
        qint64 uptime = starttimeRef.elapsed();
        if (uptime > 1000)
        {
            return uptime / 1000;
        }
    }

    return 0;
}

/*! Adds new node(s) to node cache.
    Only supported ZLL and HA nodes will be added.
    \param node - the base for the LightNode
 */
void DeRestPluginPrivate::addLightNode(const deCONZ::Node *node)
{
    DBG_Assert(node != 0);
    if (!node)
    {
        return;
    }

    QList<deCONZ::SimpleDescriptor>::const_iterator i = node->simpleDescriptors().constBegin();
    QList<deCONZ::SimpleDescriptor>::const_iterator end = node->simpleDescriptors().constEnd();

    for (;i != end; ++i)
    {
        LightNode lightNode;
        lightNode.setNode(0);
        lightNode.setIsAvailable(true);

        // check if node already exist
        LightNode *lightNode2 = getLightNodeForAddress(node->address().ext(), i->endpoint());

        if (lightNode2)
        {
            if (lightNode2->node() != node)
            {
                lightNode2->setNode(const_cast<deCONZ::Node*>(node));
                DBG_Printf(DBG_INFO, "LightNode %s set node %s\n", qPrintable(lightNode2->id()), qPrintable(node->address().toStringExt()));
            }

            lightNode2->setManufacturerCode(node->nodeDescriptor().manufacturerCode());

            if (!lightNode2->isAvailable())
            {
                // the node existed before
                // refresh all with new values
                DBG_Printf(DBG_INFO, "LightNode %u: %s updated\n", lightNode2->id().toUInt(), qPrintable(lightNode2->name()));
                lightNode2->setIsAvailable(true);
                lightNode2->setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
                lightNode2->enableRead(READ_VENDOR_NAME |
                                       READ_MODEL_ID |
                                       READ_SWBUILD_ID |
                                       READ_COLOR |
                                       READ_LEVEL |
                                       READ_ON_OFF |
                                       READ_GROUPS |
                                       READ_SCENES |
                                       READ_BINDING_TABLE);

                lightNode2->setLastRead(idleTotalCounter);
                updateEtag(lightNode2->etag);
            }

            if (lightNode2->uniqueId().isEmpty() || lightNode2->uniqueId().startsWith("0x"))
            {
                QString uid;
                union _a
                {
                    quint8 bytes[8];
                    quint64 mac;
                } a;
                a.mac = lightNode2->address().ext();
                uid.sprintf("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X-%02X",
                            a.bytes[7], a.bytes[6], a.bytes[5], a.bytes[4],
                            a.bytes[3], a.bytes[2], a.bytes[1], a.bytes[0],
                            lightNode.haEndpoint().endpoint());
                lightNode2->setUniqueId(uid);
                updateEtag(lightNode2->etag);
            }

            continue;
        }

        if (!i->inClusters().isEmpty())
        {
            if (i->profileId() == HA_PROFILE_ID)
            {
                // filter for supported devices
                switch (i->deviceId())
                {
                case DEV_ID_MAINS_POWER_OUTLET:
                case DEV_ID_HA_ONOFF_LIGHT:
                case DEV_ID_ONOFF_OUTPUT:
                case DEV_ID_HA_DIMMABLE_LIGHT:
                case DEV_ID_HA_COLOR_DIMMABLE_LIGHT:

                case DEV_ID_ZLL_ONOFF_LIGHT:
                case DEV_ID_ZLL_ONOFF_PLUGIN_UNIT:
                case DEV_ID_ZLL_ONOFF_SENSOR:
    //            case DEV_ID_ZLL_DIMMABLE_LIGHT: // same as DEV_ID_HA_ONOFF_LIGHT
                case DEV_ID_ZLL_DIMMABLE_PLUGIN_UNIT:
                case DEV_ID_ZLL_COLOR_LIGHT:
                case DEV_ID_ZLL_EXTENDED_COLOR_LIGHT:
                case DEV_ID_ZLL_COLOR_TEMPERATURE_LIGHT:
                    {
                        lightNode.setHaEndpoint(*i);
                    }
                    break;

                case DEV_ID_ZLL_COLOR_CONTROLLER:
                    {
                        // FIXME special temporary filter to detect xxx 4 key switch
                        if (i->endpoint() == 0x01)
                        {
                            int found = 0;

                            for (int ci = 0; ci < i->inClusters().size(); ci++)
                            {
                                if (i->inClusters()[ci].id() == COLOR_CLUSTER_ID ||
                                    i->inClusters()[ci].id() == LEVEL_CLUSTER_ID)
                                {
                                    found++;

                                    if (found == 2)
                                    {
                                        lightNode.setHaEndpoint(*i);
                                        lightNode.setIsOn(true);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    break;

                default:
                    {
                        DBG_Printf(DBG_INFO, "Unsupported HA deviceId 0x%04X\n", i->deviceId());
                    }
                    break;
                }
            }
            else if (i->profileId() == ZLL_PROFILE_ID)
            {
                // filter for supported devices
                switch (i->deviceId())
                {
                case DEV_ID_ZLL_COLOR_LIGHT:
                case DEV_ID_ZLL_EXTENDED_COLOR_LIGHT:
                case DEV_ID_ZLL_COLOR_TEMPERATURE_LIGHT:
                case DEV_ID_ZLL_DIMMABLE_LIGHT:
                case DEV_ID_ZLL_DIMMABLE_PLUGIN_UNIT:
                case DEV_ID_ZLL_ONOFF_LIGHT:
                case DEV_ID_ZLL_ONOFF_PLUGIN_UNIT:
                case DEV_ID_ZLL_ONOFF_SENSOR:
                    {
                        lightNode.setHaEndpoint(*i);
                    }
                    break;

                default:
                    break;
                }
            }
        }

        if (lightNode.haEndpoint().isValid())
        {
            lightNode.setNode(const_cast<deCONZ::Node*>(node));
            lightNode.address() = node->address();
            lightNode.setManufacturerCode(node->nodeDescriptor().manufacturerCode());

            QString uid;
            union _a
            {
                quint8 bytes[8];
                quint64 mac;
            } a;
            a.mac = lightNode.address().ext();
            uid.sprintf("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X-%02X",
                        a.bytes[7], a.bytes[6], a.bytes[5], a.bytes[4],
                        a.bytes[3], a.bytes[2], a.bytes[1], a.bytes[0],
                        lightNode.haEndpoint().endpoint());
            lightNode.setUniqueId(uid);

            openDb();
            loadLightNodeFromDb(&lightNode);
            closeDb();

            if (lightNode.id().isEmpty())
            {
                openDb();
                lightNode.setId(QString::number(getFreeLightId()));
                closeDb();
            }

            if (lightNode.name().isEmpty())
            {
                lightNode.setName(QString("Light %1").arg(lightNode.id()));
            }

            // force reading attributes
            lightNode.setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
            lightNode.enableRead(READ_VENDOR_NAME |
                                 READ_MODEL_ID |
                                 READ_SWBUILD_ID |
                                 READ_COLOR |
                                 READ_LEVEL |
                                 READ_ON_OFF |
                                 READ_GROUPS |
                                 READ_SCENES |
                                 READ_BINDING_TABLE);
            lightNode.setLastRead(idleTotalCounter);
            lightNode.setLastAttributeReportBind(idleTotalCounter);

            DBG_Printf(DBG_INFO, "LightNode %u: %s added\n", lightNode.id().toUInt(), qPrintable(lightNode.name()));
            nodes.push_back(lightNode);
            lightNode2 = &nodes.back();

            Q_Q(DeRestPlugin);
            q->startZclAttributeTimer(checkZclAttributesDelay);
            updateEtag(lightNode2->etag);
        }
    }
}

/*! Checks if a known node changed its reachable state changed.
    \param node - the base for the LightNode
    \return the related LightNode or 0
 */
void DeRestPluginPrivate::nodeZombieStateChanged(const deCONZ::Node *node)
{
    if (!node)
    {
        return;
    }

    bool available = !node->isZombie();

    { // lights
        std::vector<LightNode>::iterator i = nodes.begin();
        std::vector<LightNode>::iterator end = nodes.end();

        for (; i != end; ++i)
        {
            if (i->address().ext() == node->address().ext())
            {
                if (i->node() != node)
                {
                    i->setNode(const_cast<deCONZ::Node*>(node));
                    DBG_Printf(DBG_INFO, "LightNode %s set node %s\n", qPrintable(i->id()), qPrintable(node->address().toStringExt()));
                }

                if (i->isAvailable() != available)
                {
                    if (available && node->endpoints().end() == std::find(node->endpoints().begin(),
                                                                          node->endpoints().end(),
                                                                          i->haEndpoint().endpoint()))
                    {
                        available = false;
                    }

                    i->setIsAvailable(available);
                    updateEtag(i->etag);
                    updateEtag(gwConfigEtag);
                }
            }
        }
    }

    { // sensors
        std::vector<Sensor>::iterator i = sensors.begin();
        std::vector<Sensor>::iterator end = sensors.end();

        for (; i != end; ++i)
        {
            if (i->address().ext() == node->address().ext())
            {
                if (i->node() != node)
                {
                    i->setNode(const_cast<deCONZ::Node*>(node));
                    DBG_Printf(DBG_INFO, "Sensor %s set node %s\n", qPrintable(i->id()), qPrintable(node->address().toStringExt()));
                }

                checkSensorNodeReachable(&(*i));
            }
        }
    }
}

/*! Updates/adds a LightNode from a Node.
    If the node does not exist it will be created
    otherwise the values will be checked for change
    and updated in the internal representation.
    \param node - holds up to date data
    \return the updated or added LightNode
 */
LightNode *DeRestPluginPrivate::updateLightNode(const deCONZ::NodeEvent &event)
{
    if (!event.node())
    {
        return 0;
    }

    bool updated = false;
    LightNode *lightNode = getLightNodeForAddress(event.node()->address().ext(), event.endpoint());

    if (!lightNode)
    {
        // was no relevant node
        return 0;
    }

    if (lightNode->node() != event.node())
    {
        lightNode->setNode(const_cast<deCONZ::Node*>(event.node()));
        DBG_Printf(DBG_INFO, "LightNode %s set node %s\n", qPrintable(lightNode->id()), qPrintable(event.node()->address().toStringExt()));
    }

    if (lightNode->isAvailable())
    {
        if ((event.node()->state() == deCONZ::FailureState) || event.node()->isZombie())
        {
            lightNode->setIsAvailable(false);
            updated = true;
        }
    }
    else
    {
        if (event.node()->state() != deCONZ::FailureState)
        {
            lightNode->setIsAvailable(true);
            updated = true;
        }
    }

    // filter
    if ((event.profileId() != HA_PROFILE_ID) && (event.profileId() != ZLL_PROFILE_ID))
    {
        return lightNode;
    }

    QList<deCONZ::SimpleDescriptor>::const_iterator i = event.node()->simpleDescriptors().constBegin();
    QList<deCONZ::SimpleDescriptor>::const_iterator end = event.node()->simpleDescriptors().constEnd();

    for (;i != end; ++i)
    {
        if (i->endpoint() != lightNode->haEndpoint().endpoint())
        {
            continue;
        }

        if (i->inClusters().isEmpty())
        {
            continue;
        }

        if (i->profileId() == HA_PROFILE_ID)
        {
            switch(i->deviceId())
            {
            case DEV_ID_MAINS_POWER_OUTLET:
            case DEV_ID_HA_COLOR_DIMMABLE_LIGHT:
            case DEV_ID_ZLL_COLOR_LIGHT:
            case DEV_ID_ZLL_EXTENDED_COLOR_LIGHT:
            case DEV_ID_ZLL_COLOR_TEMPERATURE_LIGHT:
            case DEV_ID_HA_DIMMABLE_LIGHT:
            //case DEV_ID_ZLL_DIMMABLE_LIGHT: // same as DEV_ID_HA_ONOFF_LIGHT
            case DEV_ID_ZLL_DIMMABLE_PLUGIN_UNIT:
            case DEV_ID_HA_ONOFF_LIGHT:
            case DEV_ID_ONOFF_OUTPUT:
            case DEV_ID_ZLL_ONOFF_LIGHT:
            case DEV_ID_ZLL_ONOFF_PLUGIN_UNIT:
            case DEV_ID_ZLL_ONOFF_SENSOR:
                break;

            default:
                continue;
            }
        }
        else if (i->profileId() == ZLL_PROFILE_ID)
        {
            switch(i->deviceId())
            {
            case DEV_ID_ZLL_COLOR_LIGHT:
            case DEV_ID_ZLL_EXTENDED_COLOR_LIGHT:
            case DEV_ID_ZLL_COLOR_TEMPERATURE_LIGHT:
            case DEV_ID_ZLL_DIMMABLE_LIGHT:
            case DEV_ID_ZLL_DIMMABLE_PLUGIN_UNIT:
            case DEV_ID_ZLL_ONOFF_LIGHT:
            case DEV_ID_ZLL_ONOFF_PLUGIN_UNIT:
            case DEV_ID_ZLL_ONOFF_SENSOR:
                break;

            default:
                continue;
            }
        }
        else
        {
            continue;
        }

        // copy whole endpoint as reference
        lightNode->setHaEndpoint(*i);

        QList<deCONZ::ZclCluster>::const_iterator ic = lightNode->haEndpoint().inClusters().constBegin();
        QList<deCONZ::ZclCluster>::const_iterator endc = lightNode->haEndpoint().inClusters().constEnd();

        for (; ic != endc; ++ic)
        {
            if (ic->id() == COLOR_CLUSTER_ID && (event.clusterId() == COLOR_CLUSTER_ID))
            {
                std::vector<deCONZ::ZclAttribute>::const_iterator ia = ic->attributes().begin();
                std::vector<deCONZ::ZclAttribute>::const_iterator enda = ic->attributes().end();
                for (;ia != enda; ++ia)
                {
                    if (ia->id() == 0x0000) // current hue
                    {
                        uint8_t hue = ia->numericValue().u8;
                        if (lightNode->hue() != hue)
                        {
                            if (hue > 254)
                            {
                                hue = 254;
                            }

                            lightNode->setHue(hue);
                            updated = true;
                        }
                    }
                    else if (ia->id() == 0x0001) // current saturation
                    {
                        uint8_t sat = ia->numericValue().u8;
                        if (lightNode->saturation() != sat)
                        {
                            lightNode->setSaturation(sat);
                            updated = true;
                        }
                    }
                    else if (ia->id() == 0x0003) // current x
                    {
                        uint16_t x = ia->numericValue().u16;
                        if (lightNode->colorX() != x)
                        {
                            lightNode->setColorXY(x, lightNode->colorY());
                            updated = true;
                        }
                    }
                    else if (ia->id() == 0x0004) // current y
                    {
                        uint16_t y = ia->numericValue().u16;
                        if (lightNode->colorY() != y)
                        {
                            lightNode->setColorXY(lightNode->colorX(), y);
                            updated = true;
                        }
                    }
                    else if (ia->id() == 0x0007) // color temperature
                    {
                        uint16_t ct = ia->numericValue().u16;
                        if (lightNode->colorTemperature() != ct)
                        {
                            lightNode->setColorTemperature(ct);
                            updated = true;
                        }
                    }
                    else if (ia->id() == 0x0008) // color mode
                    {
                        uint8_t cm = ia->numericValue().u8;

                        const char *modes[3] = {"hs", "xy", "ct"};
                        if (cm < 3)
                        {
                            if (lightNode->colorMode() != modes[cm])
                            {
                                lightNode->setColorMode(modes[cm]);
                                updated = true;
                            }
                        }
                    }
                    else if (ia->id() == 0x4002) // color loop active
                    {
                        bool colorLoopActive = ia->numericValue().u8 == 0x01;

                        if (lightNode->isColorLoopActive() != colorLoopActive)
                        {
                            lightNode->setColorLoopActive(colorLoopActive);
                            updated = true;
                        }
                    }
                    else if (ia->id() == 0x4004) // color loop time
                    {
                        uint8_t clTime = ia->numericValue().u8;

                        if (lightNode->colorLoopSpeed() != clTime)
                        {
                            lightNode->setColorLoopSpeed(clTime);
                            updated = true;
                        }
                    }
                }
            }
            else if (ic->id() == LEVEL_CLUSTER_ID && (event.clusterId() == LEVEL_CLUSTER_ID))
            {
                std::vector<deCONZ::ZclAttribute>::const_iterator ia = ic->attributes().begin();
                std::vector<deCONZ::ZclAttribute>::const_iterator enda = ic->attributes().end();
                for (;ia != enda; ++ia)
                {
                    if (ia->id() == 0x0000) // current level
                    {
                        uint8_t level = ia->numericValue().u8;
                        if (lightNode->level() != level)
                        {
                            DBG_Printf(DBG_INFO, "level %u --> %u\n", lightNode->level(), level);
                            lightNode->clearRead(READ_LEVEL);
                            lightNode->setLevel(level);
                            updated = true;
                        }
                    }
                }
            }
            else if (ic->id() == ONOFF_CLUSTER_ID && (event.clusterId() == ONOFF_CLUSTER_ID))
            {
                std::vector<deCONZ::ZclAttribute>::const_iterator ia = ic->attributes().begin();
                std::vector<deCONZ::ZclAttribute>::const_iterator enda = ic->attributes().end();
                for (;ia != enda; ++ia)
                {
                    if (ia->id() == 0x0000) // OnOff
                    {
                        bool on = ia->numericValue().u8;
                        if (lightNode->isOn() != on)
                        {
                            lightNode->clearRead(READ_ON_OFF);
                            lightNode->setIsOn(on);
                            updated = true;
                        }
                    }
                }
            }
            else if (ic->id() == BASIC_CLUSTER_ID && (event.clusterId() == BASIC_CLUSTER_ID))
            {
                std::vector<deCONZ::ZclAttribute>::const_iterator ia = ic->attributes().begin();
                std::vector<deCONZ::ZclAttribute>::const_iterator enda = ic->attributes().end();
                for (;ia != enda; ++ia)
                {
                    if (ia->id() == 0x0004) // Manufacturer name
                    {
                        QString str = ia->toString();
                        if (!str.isEmpty() && str != lightNode->manufacturer())
                        {
                            lightNode->setManufacturerName(str);
                            updated = true;
                        }
                    }
                    else if (ia->id() == 0x0005) // Model identifier
                    {
                        QString str = ia->toString();
                        if (!str.isEmpty())
                        {
                            lightNode->setModelId(str);
                            updated = true;
                        }
                    }
                    else if (ia->id() == 0x4000) // Software build identifier
                    {
                        QString str = ia->toString();
                        if (!str.isEmpty())
                        {
                            lightNode->setSwBuildId(str);
                            updated = true;
                        }
                    }
                }
            }
        }

        break;
    }

    if (updated)
    {
        updateEtag(lightNode->etag);
        updateEtag(gwConfigEtag);
    }

    return lightNode;
}

/*! Returns a LightNode for a given MAC address or 0 if not found.
 */
LightNode *DeRestPluginPrivate::getLightNodeForAddress(quint64 extAddr, quint8 endpoint)
{
    std::vector<LightNode>::iterator i;
    std::vector<LightNode>::iterator end = nodes.end();

    for (i = nodes.begin(); i != end; ++i)
    {
        if (i->address().ext() == extAddr)
        {
            if ((endpoint == 0) || (endpoint == i->haEndpoint().endpoint()))
            {
                return &(*i);
            }
        }
    }

    return 0;
}

/*! Returns the number of Endpoints of a device.
 */
int DeRestPluginPrivate::getNumberOfEndpoints(quint64 extAddr)
{
    int count = 0;
    std::vector<LightNode>::iterator i;
    std::vector<LightNode>::iterator end = nodes.end();

    for (i = nodes.begin(); i != end; ++i)
    {
        if (i->address().ext() == extAddr)
        {
            count++;
        }
    }

    return count;
}

/*! Returns a LightNode for its given \p id or 0 if not found.
 */
LightNode *DeRestPluginPrivate::getLightNodeForId(const QString &id)
{
    std::vector<LightNode>::iterator i;
    std::vector<LightNode>::iterator end = nodes.end();

    for (i = nodes.begin(); i != end; ++i)
    {
        if (i->id() == id)
        {
            return &(*i);
        }
    }

    return 0;
}

/*! Returns a Rule for its given \p id or 0 if not found.
 */
Rule *DeRestPluginPrivate::getRuleForId(const QString &id)
{
    std::vector<Rule>::iterator i;
    std::vector<Rule>::iterator end = rules.end();

    for (i = rules.begin(); i != end; ++i)
    {
        if (i->id() == id && i->state() != Rule::StateDeleted)
        {
            return &(*i);
        }
    }

    end = rules.end();

    for (i = rules.begin(); i != end; ++i)
    {
        if (i->id() == id)
        {
            return &(*i);
        }
    }

    return 0;
}

/*! Returns a Rule for its given \p name or 0 if not found.
 */
Rule *DeRestPluginPrivate::getRuleForName(const QString &name)
{
    std::vector<Rule>::iterator i;
    std::vector<Rule>::iterator end = rules.end();

    for (i = rules.begin(); i != end; ++i)
    {
        if (i->name() == name)
        {
            return &(*i);
        }
    }

    return 0;
}

/*! Checks if a SensorNode is reachable.
    \param sensor - the SensorNode
 */
void DeRestPluginPrivate::checkSensorNodeReachable(Sensor *sensor)
{
    if (!sensor)
    {
        return;
    }

    bool updated = false;
    bool reachable = false;

    if (!sensor->fingerPrint().hasEndpoint())
    {
        reachable = true; // assumption for GP device
    }
    else if (sensor->node() && !sensor->node()->isZombie())
    {
        // look if fingerprint endpoint is in active endpoint list
        std::vector<quint8>::const_iterator it;

        it = std::find(sensor->node()->endpoints().begin(),
                       sensor->node()->endpoints().end(),
                       sensor->fingerPrint().endpoint);

        if (it != sensor->node()->endpoints().end())
        {
            reachable = true;
        }
    }

    if (sensor->config().reachable() != reachable)
    {
        SensorConfig sensorConfig = sensor->config();
        sensorConfig.setReachable(reachable);
        sensor->setConfig(sensorConfig);
        updated = true;
    }

    if (reachable)
    {
        if (!sensor->isAvailable())
        {
            // the node existed before
            // refresh all with new values
            DBG_Printf(DBG_INFO, "SensorNode id: %s (%s) available\n", qPrintable(sensor->id()), qPrintable(sensor->name()));
            sensor->setIsAvailable(true);
            sensor->setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
            sensor->enableRead(READ_BINDING_TABLE/* | READ_GROUP_IDENTIFIERS | READ_MODEL_ID | READ_SWBUILD_ID | READ_VENDOR_NAME*/);
            sensor->setLastRead(idleTotalCounter);
            checkSensorBindingsForAttributeReporting(sensor);
            updated = true;
        }

        if (sensor->deletedState() == Sensor::StateDeleted && gwPermitJoinDuration > 0)
        {
            DBG_Printf(DBG_INFO, "Rediscovered deleted SensorNode %s set node %s\n", qPrintable(sensor->id()), qPrintable(sensor->address().toStringExt()));
            sensor->setDeletedState(Sensor::StateNormal);
            sensor->setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
            sensor->enableRead(READ_BINDING_TABLE | READ_GROUP_IDENTIFIERS | READ_MODEL_ID | READ_VENDOR_NAME);
            sensor->setLastRead(idleTotalCounter);
            updated = true;
        }
    }
    else
    {
        if (sensor->isAvailable())
        {
            DBG_Printf(DBG_INFO, "SensorNode id: %s (%s) no longer available\n", qPrintable(sensor->id()), qPrintable(sensor->name()));
            sensor->setIsAvailable(false);
            updated = true;
        }
    }

    if (updated)
    {
        updateEtag(sensor->etag);
        updateEtag(gwConfigEtag);
        queSaveDb(DB_SENSORS, DB_SHORT_SAVE_DELAY);
    }
}

/*! Adds a new sensor node to node cache.
    Only supported ZLL and HA nodes will be added.
    \param node - the base for the SensorNode
 */
void DeRestPluginPrivate::addSensorNode(const deCONZ::Node *node)
{
    DBG_Assert(node != 0);

    if (!node)
    {
        return;
    }

    { // check existing sensors
        std::vector<Sensor>::iterator i = sensors.begin();
        std::vector<Sensor>::iterator end = sensors.end();

        for (; i != end; ++i)
        {
            if (i->address().ext() == node->address().ext())
            {
                if (i->node() != node)
                {
                    i->setNode(const_cast<deCONZ::Node*>(node));
                    DBG_Printf(DBG_INFO, "SensorNode %s set node %s\n", qPrintable(i->id()), qPrintable(node->address().toStringExt()));
                }

                // address changed?
                if (i->address().nwk() != node->address().nwk())
                {
                    i->address() = node->address();
                }
            }
        }
    }

    // check for new sensors
    QList<deCONZ::SimpleDescriptor>::const_iterator i = node->simpleDescriptors().constBegin();
    QList<deCONZ::SimpleDescriptor>::const_iterator end = node->simpleDescriptors().constEnd();

    for (;i != end; ++i)
    {
        SensorFingerprint fpSwitch;
        SensorFingerprint fpLightSensor;
        SensorFingerprint fpPresenceSensor;

        {   // scan client clusters of endpoint
            QList<deCONZ::ZclCluster>::const_iterator ci = i->outClusters().constBegin();
            QList<deCONZ::ZclCluster>::const_iterator cend = i->outClusters().constEnd();
            for (; ci != cend; ++ci)
            {
                switch (ci->id())
                {
                case ONOFF_CLUSTER_ID:
                case LEVEL_CLUSTER_ID:
                case SCENE_CLUSTER_ID:
                {
                    fpSwitch.outClusters.push_back(ci->id());
                }
                    break;

                default:
                    break;
                }
            }
        }

        {   // scan server clusters of endpoint
            QList<deCONZ::ZclCluster>::const_iterator ci = i->inClusters().constBegin();
            QList<deCONZ::ZclCluster>::const_iterator cend = i->inClusters().constEnd();
            for (; ci != cend; ++ci)
            {
                switch (ci->id())
                {
                case BASIC_CLUSTER_ID:
                {
                    fpSwitch.inClusters.push_back(ci->id());
                }
                    break;

                case COMMISSIONING_CLUSTER_ID:
                {
                    fpSwitch.inClusters.push_back(ci->id());
                }
                    break;

                case ONOFF_SWITCH_CONFIGURATION_CLUSTER_ID:
                {
                    fpSwitch.inClusters.push_back(ci->id());
                }
                    break;

                case OCCUPANCY_SENSING_CLUSTER_ID:
                {
                    fpPresenceSensor.inClusters.push_back(ci->id());
                }
                    break;

                case ILLUMINANCE_MEASUREMENT_CLUSTER_ID:
                case ILLUMINANCE_LEVEL_SENSING_CLUSTER_ID:
                {
                    fpLightSensor.inClusters.push_back(ci->id());
                }
                    break;

                default:
                    break;
                }
            }
        }

        Sensor *sensor = 0;

        // ZHASwitch
        std::vector<quint16> c = fpSwitch.inClusters;
        if ((std::find(c.begin(), c.end(), ONOFF_SWITCH_CONFIGURATION_CLUSTER_ID) != c.end()) || !fpSwitch.outClusters.empty()) // (!fpSwitch.inClusters.empty() || !fpSwitch.outClusters.empty())
        {
            fpSwitch.endpoint = i->endpoint();
            fpSwitch.deviceId = i->deviceId();
            fpSwitch.profileId = i->profileId();

            sensor = getSensorNodeForFingerPrint(node->address().ext(), fpSwitch, "ZHASwitch");

            if (!sensor)
            {
                addSensorNode(node, fpSwitch, "ZHASwitch");
            }
            else
            {
                checkSensorNodeReachable(sensor);
            }
        }

        // ZHALight
        if (!fpLightSensor.inClusters.empty() || !fpLightSensor.outClusters.empty())
        {
            fpLightSensor.endpoint = i->endpoint();
            fpLightSensor.deviceId = i->deviceId();
            fpLightSensor.profileId = i->profileId();

            sensor = getSensorNodeForFingerPrint(node->address().ext(), fpLightSensor, "ZHALight");
            if (!sensor)
            {
                addSensorNode(node, fpLightSensor, "ZHALight");
            }
            else
            {
                checkSensorNodeReachable(sensor);
            }
        }

        // ZHAPresence
        if (!fpPresenceSensor.inClusters.empty() || !fpPresenceSensor.outClusters.empty())
        {
            fpPresenceSensor.endpoint = i->endpoint();
            fpPresenceSensor.deviceId = i->deviceId();
            fpPresenceSensor.profileId = i->profileId();

            sensor = getSensorNodeForFingerPrint(node->address().ext(), fpPresenceSensor, "ZHAPresence");
            if (!sensor)
            {
                addSensorNode(node, fpPresenceSensor, "ZHAPresence");
            }
            else
            {
                sensor->setLastRead(idleTotalCounter);
                sensor->enableRead(READ_OCCUPANCY_CONFIG);
                sensor->setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
                checkSensorNodeReachable(sensor);
                Q_Q(DeRestPlugin);
                q->startZclAttributeTimer(checkZclAttributesDelay);
            }
        }
    }
}

void DeRestPluginPrivate::addSensorNode(const deCONZ::Node *node, const SensorFingerprint &fingerPrint, const QString &type)
{
    DBG_Assert(node != 0);
    if (!node)
    {
        return;
    }

    Sensor sensorNode;
    sensorNode.setIsAvailable(true);
    sensorNode.setNode(const_cast<deCONZ::Node*>(node));
    sensorNode.address() = node->address();
    sensorNode.setType(type);
    sensorNode.setUniqueId(node->address().toStringExt());
    sensorNode.fingerPrint() = fingerPrint;

    SensorConfig sensorConfig;
    sensorConfig.setReachable(true);
    sensorNode.setConfig(sensorConfig);

    if (node->nodeDescriptor().manufacturerCode() == VENDOR_DDEL)
    {
        sensorNode.setManufacturer("dresden elektronik");
    }
    else if ((node->nodeDescriptor().manufacturerCode() == VENDOR_OSRAM_STACK) || (node->nodeDescriptor().manufacturerCode() == VENDOR_OSRAM))
    {
        sensorNode.setManufacturer("OSRAM");
    }
    else if (node->nodeDescriptor().manufacturerCode() == VENDOR_UBISYS)
    {
        sensorNode.setManufacturer("Ubisys");
    }
    else if (node->nodeDescriptor().manufacturerCode() == VENDOR_BUSCH_JAEGER)
    {
        sensorNode.setManufacturer("Busch Jaeger");
    }
    else if (node->nodeDescriptor().manufacturerCode() == VENDOR_PHILIPS)
    {
        sensorNode.setManufacturer("Philips");
    }
    else if (node->nodeDescriptor().manufacturerCode() == VENDOR_BEGA)
    {
        sensorNode.setManufacturer("BEGA Gantenbrink-Leuchten KG");
    }
    openDb();
    loadSensorNodeFromDb(&sensorNode);
    closeDb();

    if (sensorNode.id().isEmpty())
    {
        openDb();
        sensorNode.setId(QString::number(getFreeSensorId()));
        closeDb();
    }

    if (sensorNode.name().isEmpty())
    {
        if (type == "ZHASwitch")
        {
            sensorNode.setName(QString("Switch %1").arg(sensorNode.id()));
        }
        else
        {
            sensorNode.setName(QString("%1 %2").arg(type).arg(sensorNode.id()));
        }
    }

    // force reading attributes
    sensorNode.setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
    sensorNode.enableRead(READ_BINDING_TABLE);
    sensorNode.setLastRead(idleTotalCounter);
    {
        std::vector<quint16>::const_iterator ci = fingerPrint.inClusters.begin();
        std::vector<quint16>::const_iterator cend = fingerPrint.inClusters.end();
        for (;ci != cend; ++ci)
        {
            if (*ci == OCCUPANCY_SENSING_CLUSTER_ID)
            {
                sensorNode.setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
                sensorNode.enableRead(READ_OCCUPANCY_CONFIG);
                sensorNode.setLastRead(idleTotalCounter);
            }
            else if (*ci == COMMISSIONING_CLUSTER_ID)
            {
                DBG_Printf(DBG_INFO, "SensorNode %u: %s read group identifiers\n", sensorNode.id().toUInt(), qPrintable(sensorNode.name()));
                sensorNode.setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
                sensorNode.enableRead(READ_GROUP_IDENTIFIERS);
                sensorNode.setLastRead(idleTotalCounter);
            }
            else if (*ci == BASIC_CLUSTER_ID)
            {
                DBG_Printf(DBG_INFO, "SensorNode %u: %s read model id and vendor name\n", sensorNode.id().toUInt(), qPrintable(sensorNode.name()));
                sensorNode.setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
                sensorNode.enableRead(READ_MODEL_ID | READ_VENDOR_NAME);
                sensorNode.setLastRead(idleTotalCounter);
            }
        }
    }

    DBG_Printf(DBG_INFO, "SensorNode %u: %s added\n", sensorNode.id().toUInt(), qPrintable(sensorNode.name()));
    updateEtag(sensorNode.etag);

    sensors.push_back(sensorNode);

    checkSensorBindingsForAttributeReporting(&sensors.back());

    Q_Q(DeRestPlugin);
    q->startZclAttributeTimer(checkZclAttributesDelay);

    queSaveDb(DB_SENSORS , DB_SHORT_SAVE_DELAY);
}

/*! Updates/adds a SensorNode from a Node.
    If the node does not exist it will be created
    otherwise the values will be checked for change
    and updated in the internal representation.
    \param node - holds up to date data
 */
void DeRestPluginPrivate::updateSensorNode(const deCONZ::NodeEvent &event)
{
    if (!event.node())
    {
        return;
    }

    bool updated = false;

    std::vector<Sensor>::iterator i = sensors.begin();
    std::vector<Sensor>::iterator end = sensors.end();

    for (; i != end; ++i)
    {
        if (i->address().ext() != event.node()->address().ext())
        {
            continue;
        }

        if (i->node() != event.node())
        {
            i->setNode(const_cast<deCONZ::Node*>(event.node()));
            DBG_Printf(DBG_INFO, "Sensor %s set node %s\n", qPrintable(i->id()), qPrintable(event.node()->address().toStringExt()));
        }

        checkSensorNodeReachable(&(*i));

        if (!i->isAvailable())
        {
            continue;
        }

        if (event.event() == deCONZ::NodeEvent::UpdatedPowerDescriptor)
        {
            if (event.node()->powerDescriptor().isValid())
            {
                SensorConfig config = i->config();

                if (event.node()->powerDescriptor().currentPowerSource() == deCONZ::PowerSourceRechargeable ||
                    event.node()->powerDescriptor().currentPowerSource() == deCONZ::PowerSourceDisposable)
                {
                    switch (event.node()->powerDescriptor().currentPowerLevel())
                    {
                    case deCONZ::PowerLevel100:      config.setBattery(100); break;
                    case deCONZ::PowerLevel66:       config.setBattery(66); break;
                    case deCONZ::PowerLevel33:       config.setBattery(33); break;
                    case deCONZ::PowerLevelCritical: config.setBattery(0); break;
                    default:
                        config.setBattery(255); // invalid
                        break;
                    }
                }
                else
                {
                    config.setBattery(255); // invalid
                }

                i->setConfig(config);
                updateEtag(i->etag);
                updateEtag(gwConfigEtag);
            }
            return;
        }

        // filter for relevant clusters
        if (event.profileId() == HA_PROFILE_ID || event.profileId() == ZLL_PROFILE_ID)
        {
            switch (event.clusterId())
            {
            case ILLUMINANCE_MEASUREMENT_CLUSTER_ID:
            case OCCUPANCY_SENSING_CLUSTER_ID:
            case BASIC_CLUSTER_ID:
                break;

            default:
                continue; // don't process further
            }
        }
        else
        {
            continue;
        }

        // filter endpoint
        if (event.endpoint() != i->fingerPrint().endpoint)
        {
            continue;
        }


        if (event.clusterId() != BASIC_CLUSTER_ID)
        { // assume data must be in server cluster attribute
            bool found = false;
            std::vector<quint16>::const_iterator ci = i->fingerPrint().inClusters.begin();
            std::vector<quint16>::const_iterator cend = i->fingerPrint().inClusters.end();
            for (; ci != cend; ++ci)
            {
                if (*ci == event.clusterId())
                {
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                continue;
            }
        }

        deCONZ::SimpleDescriptor sd;
        if (event.node()->copySimpleDescriptor(event.endpoint(), &sd) == 0)
        {
            QList<deCONZ::ZclCluster>::const_iterator ic = sd.inClusters().constBegin();
            QList<deCONZ::ZclCluster>::const_iterator endc = sd.inClusters().constEnd();

            for (; ic != endc; ++ic)
            {
                if (ic->id() == event.clusterId())
                {
                    std::vector<deCONZ::ZclAttribute>::const_iterator ia = ic->attributes().begin();
                    std::vector<deCONZ::ZclAttribute>::const_iterator enda = ic->attributes().end();

                    NodeValue::UpdateType updateType = NodeValue::UpdateInvalid;
                    if (event.event() == deCONZ::NodeEvent::UpdatedClusterDataZclRead)
                    {
                        updateType = NodeValue::UpdateByZclRead;
                    }
                    else if (event.event() == deCONZ::NodeEvent::UpdatedClusterDataZclReport)
                    {
                        updateType = NodeValue::UpdateByZclReport;
                    }

                    if (event.clusterId() == ILLUMINANCE_MEASUREMENT_CLUSTER_ID)
                    {
                        for (;ia != enda; ++ia)
                        {
                            if (ia->id() == 0x0000) // measured illuminance (lux)
                            {
                                if (updateType != NodeValue::UpdateInvalid)
                                {
                                    i->setZclValue(updateType, event.clusterId(), 0x0000, ia->numericValue());
                                }

                                quint32 lux = ia->numericValue().u16; // ZigBee uses a 16-bit value

                                if (i->modelId().startsWith("FLS-NB"))
                                {
                                    // TODO check firmware version
                                }
                                else if (lux > 0 && lux < 0xffff)
                                {
                                    // valid values are 1 - 0xfffe
                                    // 0, too low to measure
                                    // 0xffff invalid value

                                    // ZCL Attribute = 10.000 * log10(Illuminance (lx)) + 1
                                    // lux = 10^(ZCL Attribute/10.000) - 1
                                    qreal exp = lux;
                                    qreal l = qPow(10, exp / 10000.0f);

                                    if (l >= 1)
                                    {
                                        l -= 1;
                                        lux = static_cast<quint32>(l);
                                    }
                                    else
                                    {
                                        DBG_Printf(DBG_INFO, "invalid lux value %u", lux);
                                        lux = 0xffff; // invalid value
                                    }
                                }

                                i->state().updateTime();
                                if (i->state().lux() != lux)
                                {
                                    i->state().setLux(lux);
                                    updateEtag(i->etag);
                                    updateEtag(gwConfigEtag);
//                                    updated = true;
                                }
                            }
                        }
                    }
                    else if (event.clusterId() == OCCUPANCY_SENSING_CLUSTER_ID)
                    {
                        for (;ia != enda; ++ia)
                        {
                            if (ia->id() == 0x0000) // occupied state
                            {
                                if (updateType != NodeValue::UpdateInvalid)
                                {
                                    i->setZclValue(updateType, event.clusterId(), 0x0000, ia->numericValue());
                                }
                            }
                            else if (ia->id() == 0x0010) // occupied to unoccupied delay
                            {
                                double duration = (double)ia->numericValue().u16;

                                if (i->config().duration() != duration)
                                {
                                    if (i->config().duration() <= 0)
                                    {
                                        DBG_Printf(DBG_INFO, "got occupied to unoccupied delay %u\n", ia->numericValue().u16);
                                        SensorConfig config = i->config();
                                        config.setDuration(duration);
                                        i->setConfig(config);
                                        updateEtag(i->etag);
                                        updated = true;
                                    }
                                    else
                                    {
                                        DBG_Printf(DBG_INFO, "occupied to unoccupied delay is %u should be %u, force rewrite\n", ia->numericValue().u16, (quint16)i->config().duration());
                                        i->enableRead(WRITE_OCCUPANCY_CONFIG);
                                        i->enableRead(READ_OCCUPANCY_CONFIG);
                                        i->setNextReadTime(QTime::currentTime());
                                        Q_Q(DeRestPlugin);
                                        q->startZclAttributeTimer(checkZclAttributesDelay);
                                    }
                                }
                            }
                        }
                    }
                    else if (event.clusterId() == BASIC_CLUSTER_ID)
                    {
                        DBG_Printf(DBG_INFO, "Update Sensor 0x%016llX Basic Cluster\n", event.node()->address().ext());
                        for (;ia != enda; ++ia)
                        {
                            if (ia->id() == 0x0005) // Model identifier
                            {
                                if (i->mustRead(READ_MODEL_ID))
                                {
                                    i->clearRead(READ_MODEL_ID);
                                }

                                QString str = ia->toString();
                                if (!str.isEmpty())
                                {
                                    if (i->modelId() != str)
                                    {
                                        i->setModelId(str);
                                        updated = true;
                                    }

                                    if (i->name() == QString("Switch %1").arg(i->id()))
                                    {
                                        QString name = QString("%1 %2").arg(str).arg(i->id());
                                        if (i->name() != name)
                                        {
                                            i->setName(name);
                                            updated = true;
                                        }
                                    }
                                }
                            }
                            if (ia->id() == 0x0004) // Manufacturer Name
                            {
                                if (i->mustRead(READ_VENDOR_NAME))
                                {
                                    i->clearRead(READ_VENDOR_NAME);
                                }

                                QString str = ia->toString();
                                if (!str.isEmpty())
                                {
                                    if (i->manufacturer() != str)
                                    {
                                        i->setManufacturer(str);
                                        updated = true;
                                    }
                                }
                            }
                            else if (ia->id() == 0x4000) // Software build identifier
                            {
                                if (i->mustRead(READ_SWBUILD_ID))
                                {
                                    i->clearRead(READ_SWBUILD_ID);
                                }
                                QString str = ia->toString();
                                if (!str.isEmpty())
                                {
                                    if (str != i->swVersion())
                                    {
                                        i->setSwVersion(str);
                                        updated = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (updated)
    {
        updateEtag(gwConfigEtag);
        queSaveDb(DB_SENSORS , DB_SHORT_SAVE_DELAY);
    }
}

/*! Checks all sensors if they are available.
 */
void DeRestPluginPrivate::checkAllSensorsAvailable()
{
    std::vector<Sensor>::iterator i = sensors.begin();
    std::vector<Sensor>::iterator end = sensors.end();

    for (; i != end; ++i)
    {
        checkSensorNodeReachable(&(*i));
    }
}

/*! Returns the first Sensor for its given \p id or 0 if not found.
    \note There might be more sensors with the same extAddr.
 */
Sensor *DeRestPluginPrivate::getSensorNodeForAddress(quint64 extAddr)
{
    std::vector<Sensor>::iterator i = sensors.begin();
    std::vector<Sensor>::iterator end = sensors.end();

    for (; i != end; ++i)
    {
        if (i->address().ext() == extAddr && i->deletedState() != Sensor::StateDeleted)
        {
            return &(*i);
        }
    }

    end = sensors.end();

    for (i = sensors.begin(); i != end; ++i)
    {
        if (i->address().ext() == extAddr)
        {
            return &(*i);
        }
    }

    return 0;

}

/*! Returns the first Sensor for its given \p extAddress and \p Endpoint or 0 if not found.
 */
Sensor *DeRestPluginPrivate::getSensorNodeForAddressAndEndpoint(quint64 extAddr, quint8 ep)
{
    std::vector<Sensor>::iterator i = sensors.begin();
    std::vector<Sensor>::iterator end = sensors.end();

    for (; i != end; ++i)
    {
        if (i->address().ext() == extAddr && ep == i->fingerPrint().endpoint && i->deletedState() != Sensor::StateDeleted)
        {
            return &(*i);
        }
    }

    end = sensors.end();

    for (i = sensors.begin(); i != end; ++i)
    {
        if (i->address().ext() == extAddr && ep == i->fingerPrint().endpoint)
        {
            return &(*i);
        }
    }

    return 0;

}

/*! Returns the first Sensor which matches a fingerprint.
    \note There might be more sensors with the same fingerprint.
 */
Sensor *DeRestPluginPrivate::getSensorNodeForFingerPrint(quint64 extAddr, const SensorFingerprint &fingerPrint, const QString &type)
{
    std::vector<Sensor>::iterator i = sensors.begin();
    std::vector<Sensor>::iterator end = sensors.end();

    for (; i != end; ++i)
    {
        if (i->address().ext() == extAddr && i->deletedState() != Sensor::StateDeleted)
        {
            if (i->type() == type && i->fingerPrint().endpoint == fingerPrint.endpoint)
            {
                if (!(i->fingerPrint() == fingerPrint))
                {
                    DBG_Printf(DBG_INFO, "updated fingerprint for sensor %s\n", qPrintable(i->name()));
                    i->fingerPrint() = fingerPrint;
                    updateEtag(i->etag);
                    queSaveDb(DB_SENSORS , DB_SHORT_SAVE_DELAY);
                }
                return &(*i);
            }
        }
    }

    end = sensors.end();

    for (i = sensors.begin(); i != end; ++i)
    {
        if (i->address().ext() == extAddr)
        {
            if (i->type() == type && i->fingerPrint().endpoint == fingerPrint.endpoint)
            {
                if (!(i->fingerPrint() == fingerPrint))
                {
                    DBG_Printf(DBG_INFO, "updated fingerprint for sensor %s\n", qPrintable(i->name()));
                    i->fingerPrint() = fingerPrint;
                    updateEtag(i->etag);
                    queSaveDb(DB_SENSORS , DB_SHORT_SAVE_DELAY);
                }
                return &(*i);
            }
        }
    }

    return 0;
}

/*! Returns a Sensor for its given \p unique id or 0 if not found.
 */
Sensor *DeRestPluginPrivate::getSensorNodeForUniqueId(const QString &uniqueId)
{
    std::vector<Sensor>::iterator i;
    std::vector<Sensor>::iterator end = sensors.end();

    for (i = sensors.begin(); i != end; ++i)
    {
        if (i->uniqueId() == uniqueId)
        {
            return &(*i);
        }
    }

    return 0;
}

/*! Returns a Sensor for its given \p id or 0 if not found.
 */
Sensor *DeRestPluginPrivate::getSensorNodeForId(const QString &id)
{
    std::vector<Sensor>::iterator i;
    std::vector<Sensor>::iterator end = sensors.end();

    for (i = sensors.begin(); i != end; ++i)
    {
        if (i->id() == id)
        {
            return &(*i);
        }
    }

    return 0;
}

/*! Returns a Group for a given group id or 0 if not found.
 */
Group *DeRestPluginPrivate::getGroupForId(uint16_t id)
{
    std::vector<Group>::iterator i = groups.begin();
    std::vector<Group>::iterator end = groups.end();

    for (; i != end; ++i)
    {
        if (i->address() == id)
        {
            return &(*i);
        }
    }

    return 0;
}

/*! Returns a Scene for a given group id and Scene id or 0 if not found.
 */
Scene *DeRestPluginPrivate::getSceneForId(uint16_t gid, uint8_t sid)
{
    Group *group = getGroupForId(gid);

    std::vector<Scene>::iterator i = group->scenes.begin();
    std::vector<Scene>::iterator end = group->scenes.end();

    for (; i != end; ++i)
    {
        if (i->id == sid)
        {
            return &(*i);
        }
    }

    return 0;
}

/*! Returns a Group for a given group name or 0 if not found.
 */
Group *DeRestPluginPrivate::getGroupForName(const QString &name)
{
    DBG_Assert(name.isEmpty() == false);
    if (name.isEmpty())
    {
        return 0;
    }

    std::vector<Group>::iterator i = groups.begin();
    std::vector<Group>::iterator end = groups.end();

    for (; i != end; ++i)
    {
        if (i->name() == name)
        {
            return &(*i);
        }
    }

    return 0;
}

/*! Returns a Group for a given group id or 0 if not found.
 */
Group *DeRestPluginPrivate::getGroupForId(const QString &id)
{
    DBG_Assert(id.isEmpty() == false);
    if (id.isEmpty())
    {
        return 0;
    }

    // check valid 16-bit group id 0..0xFFFF
    bool ok;
    uint gid = id.toUInt(&ok, 10);
    if (!ok || (gid > 0xFFFFUL))
    {
        DBG_Printf(DBG_INFO, "Get group for id error: invalid group id %s\n", qPrintable(id));
        return 0;
    }

    std::vector<Group>::iterator i = groups.begin();
    std::vector<Group>::iterator end = groups.end();

    for (; i != end; ++i)
    {
        if (i->id() == id)
        {
            return &(*i);
        }
    }

    return 0;
}

/*! Returns GroupInfo in a LightNode for a given group id or 0 if not found.
 */
GroupInfo *DeRestPluginPrivate::getGroupInfo(LightNode *lightNode, uint16_t id)
{
    DBG_Assert(lightNode != 0);

    if (lightNode)
    {
        std::vector<GroupInfo>::iterator i = lightNode->groups().begin();
        std::vector<GroupInfo>::iterator end = lightNode->groups().end();

        for (; i != end; ++i)
        {
            if (i->id == id)
            {
                return &(*i);
            }
        }
    }

    return 0;
}

/*! Returns a GroupInfo in a LightNode for a given group (will be created if not exist).
 */
GroupInfo *DeRestPluginPrivate::createGroupInfo(LightNode *lightNode, uint16_t id)
{
    DBG_Assert(lightNode != 0);

    // dont create a duplicate
    GroupInfo *g = getGroupInfo(lightNode, id);
    if (g)
    {
        return g;
    }

    // not found .. create
    GroupInfo groupInfo;
    groupInfo.id = id;
    lightNode->groups().push_back(groupInfo);

    return &lightNode->groups().back();
}

/*! Returns a deCONZ::Node for a given MAC address or 0 if not found.
 */
deCONZ::Node *DeRestPluginPrivate::getNodeForAddress(uint64_t extAddr)
{
    int i = 0;
    const deCONZ::Node *node;

    DBG_Assert(apsCtrl != 0);

    if (apsCtrl == 0)
    {
        return 0;
    }

    while (apsCtrl->getNode(i, &node) == 0)
    {
        if (node->address().ext() == extAddr)
        {
            return const_cast<deCONZ::Node*>(node); // FIXME: use const
        }
        i++;
    }

    return 0;
}

/*! Returns the cluster descriptor for given cluster id.
    \return the cluster or 0 if not found
 */
deCONZ::ZclCluster *DeRestPluginPrivate::getInCluster(deCONZ::Node *node, uint8_t endpoint, uint16_t clusterId)
{
    if (DBG_Assert(node != 0) == false)
    {
        return 0;
    }

    deCONZ::SimpleDescriptor *sd = node->getSimpleDescriptor(endpoint);

    if (sd)
    {
        QList<deCONZ::ZclCluster>::iterator i = sd->inClusters().begin();
        QList<deCONZ::ZclCluster>::iterator end = sd->inClusters().end();

        for (; i != end; ++i)
        {
            if (i->id() == clusterId)
            {
                return &(*i);
            }
        }
    }

    return 0;
}

/*! Get proper src endpoint for outgoing requests.
    \param req - the profileId() must be specified in the request.
    \return a endpoint number
 */
uint8_t DeRestPluginPrivate::getSrcEndpoint(RestNodeBase *restNode, const deCONZ::ApsDataRequest &req)
{
    Q_UNUSED(restNode);
    if (req.profileId() == HA_PROFILE_ID || req.profileId() == ZLL_PROFILE_ID)
    {
        return endpoint();
    }
    return 0x01;
}

/*! Check and process queued attributes marked for read.
    \return true - if at least one attribute was processed
 */
bool DeRestPluginPrivate::processZclAttributes(LightNode *lightNode)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode)
    {
        return false;
    }

    // check if read should happen now
    if (lightNode->nextReadTime() > QTime::currentTime())
    {
        return false;
    }

    if (!lightNode->isAvailable())
    {
        return false;
    }

    deCONZ::ApsController *apsCtrl = deCONZ::ApsController::instance();
    DBG_Assert(apsCtrl != 0);
    if (apsCtrl && (apsCtrl->getParameter(deCONZ::ParamAutoPollingActive) == 0))
    {
        return false;
    }

    int processed = 0;
    bool readColor = false;
    bool readLevel = false;
    bool readOnOff = false;

    if (lightNode->haEndpoint().profileId() == ZLL_PROFILE_ID)
    {
        switch(lightNode->haEndpoint().deviceId())
        {
        case DEV_ID_ZLL_COLOR_LIGHT:
        case DEV_ID_ZLL_EXTENDED_COLOR_LIGHT:
        case DEV_ID_ZLL_COLOR_TEMPERATURE_LIGHT:
            readColor = true;
            //fall through

        case DEV_ID_ZLL_DIMMABLE_LIGHT:
        case DEV_ID_ZLL_DIMMABLE_PLUGIN_UNIT:
            readLevel = true;
            //fall through

        case DEV_ID_ZLL_ONOFF_LIGHT:
        case DEV_ID_ZLL_ONOFF_PLUGIN_UNIT:
        case DEV_ID_ZLL_ONOFF_SENSOR:
            readOnOff = true;
            break;

        default:
            break;
        }
    }
    else if (lightNode->haEndpoint().profileId() == HA_PROFILE_ID)
    {
        switch(lightNode->haEndpoint().deviceId())
        {
        case DEV_ID_HA_COLOR_DIMMABLE_LIGHT:
        case DEV_ID_ZLL_COLOR_LIGHT:
        case DEV_ID_ZLL_EXTENDED_COLOR_LIGHT:
        case DEV_ID_ZLL_COLOR_TEMPERATURE_LIGHT:
            readColor = true;
            //fall through

        case DEV_ID_HA_DIMMABLE_LIGHT:
        //case DEV_ID_ZLL_DIMMABLE_LIGHT: // same as DEV_ID_HA_ONOFF_LIGHT
        case DEV_ID_ZLL_DIMMABLE_PLUGIN_UNIT:
            readLevel = true;
            //fall through

        case DEV_ID_MAINS_POWER_OUTLET:
        case DEV_ID_HA_ONOFF_LIGHT:
        case DEV_ID_ZLL_ONOFF_LIGHT:
        case DEV_ID_ZLL_ONOFF_PLUGIN_UNIT:
        case DEV_ID_ZLL_ONOFF_SENSOR:
            readOnOff = true;
            break;

        default:
            break;
        }
    }

    if (lightNode->mustRead(READ_BINDING_TABLE))
    {
        if (readBindingTable(lightNode, 0))
        {
            // only read binding table once per node even if multiple devices/sensors are implemented
            std::vector<LightNode>::iterator i = nodes.begin();
            std::vector<LightNode>::iterator end = nodes.end();

            for (; i != end; ++i)
            {
                if (i->address().ext() == lightNode->address().ext())
                {
                    i->clearRead(READ_BINDING_TABLE);
                }
            }
            processed++;
        }
    }

    if (lightNode->mustRead(READ_VENDOR_NAME))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x0004); // Manufacturer name

        if (readAttributes(lightNode, lightNode->haEndpoint().endpoint(), BASIC_CLUSTER_ID, attributes))
        {
            lightNode->clearRead(READ_VENDOR_NAME);
            processed++;
        }
    }

    if ((processed < 2) && lightNode->mustRead(READ_MODEL_ID))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x0005); // Model identifier

        if (readAttributes(lightNode, lightNode->haEndpoint().endpoint(), BASIC_CLUSTER_ID, attributes))
        {
            lightNode->clearRead(READ_MODEL_ID);
            processed++;
        }
    }

    if ((processed < 2) && lightNode->mustRead(READ_SWBUILD_ID))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x4000); // Software build identifier

        if (readAttributes(lightNode, lightNode->haEndpoint().endpoint(), BASIC_CLUSTER_ID, attributes))
        {
            lightNode->clearRead(READ_SWBUILD_ID);
            processed++;
        }
    }

    if ((processed < 2) && readOnOff && lightNode->mustRead(READ_ON_OFF))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x0000); // OnOff

        if (readAttributes(lightNode, lightNode->haEndpoint().endpoint(), ONOFF_CLUSTER_ID, attributes))
        {
            lightNode->clearRead(READ_ON_OFF);
            processed++;
        }
    }

    if ((processed < 2) && readLevel && lightNode->mustRead(READ_LEVEL))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x0000); // Level

        if (readAttributes(lightNode, lightNode->haEndpoint().endpoint(), LEVEL_CLUSTER_ID, attributes))
        {
            lightNode->clearRead(READ_LEVEL);
            processed++;
        }
    }

    if ((processed < 2) && readColor && lightNode->mustRead(READ_COLOR))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x0000); // Current hue
        attributes.push_back(0x0001); // Current saturation
        attributes.push_back(0x0003); // Current x
        attributes.push_back(0x0004); // Current y
        attributes.push_back(0x0007); // Color temperature
        attributes.push_back(0x0008); // Color mode
        attributes.push_back(0x4000); // Enhanced hue
        attributes.push_back(0x4002); // Color loop active

        if (readAttributes(lightNode, lightNode->haEndpoint().endpoint(), COLOR_CLUSTER_ID, attributes))
        {
            lightNode->clearRead(READ_COLOR);
            processed++;
        }
    }

    if ((processed < 2) && lightNode->mustRead(READ_GROUPS))
    {
        std::vector<uint16_t> groups; // empty meaning read all groups
        if (readGroupMembership(lightNode, groups))
        {
            lightNode->clearRead(READ_GROUPS);
            processed++;
        }
    }

    if ((processed < 2) && lightNode->mustRead(READ_SCENES) && !lightNode->groups().empty())
    {
        std::vector<GroupInfo>::iterator i = lightNode->groups().begin();
        std::vector<GroupInfo>::iterator end = lightNode->groups().end();

        int rd = 0;

        for (; i != end; ++i)
        {
            Group *group = getGroupForId(i->id);

            if (group && group->state() != Group::StateDeleted && group->state() != Group::StateDeleteFromDB)
            {
                // NOTE: this may cause problems if we have a lot of nodes + groups
                // proposal mark groups for which scenes where discovered
                if (readSceneMembership(lightNode, group))
                {
                    processed++;
                    rd++;
                }
                else
                {
                    // print but don't take action
                    DBG_Printf(DBG_INFO_L2, "read scenes membership for group: 0x%04X rejected\n", i->id);
                }
            }
        }

        if (!lightNode->groups().empty())
        {
            if (rd > 0)
            {
                lightNode->clearRead(READ_SCENES);
            }
        }
        else
        {
            lightNode->clearRead(READ_SCENES);
        }

    }

    if ((processed < 2) && lightNode->mustRead(READ_SCENE_DETAILS))
    {
        std::vector<GroupInfo>::iterator g = lightNode->groups().begin();
        std::vector<GroupInfo>::iterator gend = lightNode->groups().end();

        int rd = 0;

        for (; g != gend; ++g)
        {
            Group *group = getGroupForId(g->id);

            if (group  && group->state() != Group::StateDeleted && group->state() != Group::StateDeleteFromDB)
            {
                std::vector<Scene>::iterator s = group->scenes.begin();
                std::vector<Scene>::iterator send = group->scenes.end();

                for (; s != send; ++s)
                {
                    if (readSceneAttributes(lightNode, g->id, s->id))
                    {
                        processed++;
                        rd++;
                    }
                    else
                    {
                        // print but don't take action
                        DBG_Printf(DBG_INFO_L2, "read scene Attributes for group: 0x%04X rejected\n", g->id);
                    }
                }
            }
        }

        if (!lightNode->groups().empty())
        {
            if (rd > 0)
            {
                lightNode->clearRead(READ_SCENE_DETAILS);
            }
        }
        else
        {
            lightNode->clearRead(READ_SCENE_DETAILS);
        }

    }

    return (processed > 0);
}

/*! Check and process queued attributes marked for read and write.
    \return true - if at least one attribute was processed
 */
bool DeRestPluginPrivate::processZclAttributes(Sensor *sensorNode)
{
    int processed = 0;

    DBG_Assert(sensorNode != 0);

    if (!sensorNode)
    {
        return false;
    }

    // check if read should happen now
    if (sensorNode->nextReadTime() > QTime::currentTime())
    {
        return false;
    }

    if (!sensorNode->isAvailable())
    {
        return false;
    }

    if (sensorNode->node() && sensorNode->node()->simpleDescriptors().isEmpty())
    {
        return false;
    }

//    deCONZ::ApsController *apsCtrl = deCONZ::ApsController::instance();
//    DBG_Assert(apsCtrl != 0);
//    if (apsCtrl && (apsCtrl->getParameter(deCONZ::ParamAutoPollingActive) == 0))
//    {
//        return false;
//    }

    if (sensorNode->mustRead(READ_BINDING_TABLE))
    {
        bool ok = false;
        // only read binding table of chosen sensors
        // whitelist by Model ID
        if (sensorNode->modelId().startsWith("FLS-NB") ||
            sensorNode->modelId().startsWith("D1") || sensorNode->modelId().startsWith("S1") ||
            sensorNode->modelId().startsWith("S2") || sensorNode->manufacturer().startsWith("BEGA") ||
            sensorNode->modelId().startsWith("C4") ||
            sensorNode->modelId().startsWith("LM_00.00"))
        {
            ok = true;
        }

        if (!ok)
        {
            sensorNode->clearRead(READ_BINDING_TABLE);
        }

        if (ok && readBindingTable(sensorNode, 0))
        {
            // only read binding table once per node even if multiple devices/sensors are implemented
            std::vector<Sensor>::iterator i = sensors.begin();
            std::vector<Sensor>::iterator end = sensors.end();

            for (; i != end; ++i)
            {
                if (i->address().ext() == sensorNode->address().ext())
                {
                    i->clearRead(READ_BINDING_TABLE);
                }
            }
            processed++;
        }
    }

    if (sensorNode->mustRead(READ_VENDOR_NAME))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x0004); // Manufacturer name

        if (readAttributes(sensorNode, sensorNode->fingerPrint().endpoint, BASIC_CLUSTER_ID, attributes))
        {
            sensorNode->clearRead(READ_VENDOR_NAME);
            processed++;
        }
    }

    if (sensorNode->mustRead(READ_MODEL_ID))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x0005); // Model identifier

        if (readAttributes(sensorNode, sensorNode->fingerPrint().endpoint, BASIC_CLUSTER_ID, attributes))
        {
            sensorNode->clearRead(READ_MODEL_ID);
            processed++;
        }
    }

    if (sensorNode->mustRead(READ_SWBUILD_ID))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x4000); // Software build identifier

        if (readAttributes(sensorNode, sensorNode->fingerPrint().endpoint, BASIC_CLUSTER_ID, attributes))
        {
            sensorNode->clearRead(READ_SWBUILD_ID);
            processed++;
        }
    }

    if (sensorNode->mustRead(READ_GROUP_IDENTIFIERS))
    {
        if (sensorNode->modelId() != "RWL021" &&
            std::find(sensorNode->fingerPrint().inClusters.begin(),
                      sensorNode->fingerPrint().inClusters.end(), COMMISSIONING_CLUSTER_ID)
                   == sensorNode->fingerPrint().inClusters.end())
        {
            // if the sensor is not a RWL021 && has no commissioning cluster
            // disable reading of group identifiers here
            sensorNode->clearRead(READ_GROUP_IDENTIFIERS);
        }
        else if (getGroupIdentifiers(sensorNode, sensorNode->fingerPrint().endpoint, 0))
        {
            sensorNode->clearRead(READ_GROUP_IDENTIFIERS);
            processed++;
        }
    }

    if (sensorNode->mustRead(READ_OCCUPANCY_CONFIG))
    {
        std::vector<uint16_t> attributes;
        attributes.push_back(0x0010); // occupied to unoccupied delay

        if (readAttributes(sensorNode, sensorNode->fingerPrint().endpoint, OCCUPANCY_SENSING_CLUSTER_ID, attributes))
        {
            sensorNode->clearRead(READ_OCCUPANCY_CONFIG);
            processed++;
        }
    }

    if (sensorNode->mustRead(WRITE_OCCUPANCY_CONFIG))
    {
        // only valid bounds
        if (sensorNode->config().duration() >= 0 && sensorNode->config().duration() <= 65535)
        {
            // occupied to unoccupied delay
            deCONZ::ZclAttribute attr(0x0010, deCONZ::Zcl16BitUint, "occ", deCONZ::ZclReadWrite, true);
            attr.setValue((quint64)sensorNode->config().duration());

            if (writeAttribute(sensorNode, sensorNode->fingerPrint().endpoint, OCCUPANCY_SENSING_CLUSTER_ID, attr))
            {
                sensorNode->clearRead(WRITE_OCCUPANCY_CONFIG);
                processed++;
            }
        }
        else
        {
            sensorNode->clearRead(WRITE_OCCUPANCY_CONFIG);
        }
    }

    return (processed > 0);
}

/*! Queue reading ZCL attributes of a node.
    \param restNode the node from which the attributes shall be read
    \param endpoint the destination endpoint
    \param clusterId the cluster id related to the attributes
    \param attributes a list of attribute ids which shall be read
    \return true if the request is queued
 */
bool DeRestPluginPrivate::readAttributes(RestNodeBase *restNode, quint8 endpoint, uint16_t clusterId, const std::vector<uint16_t> &attributes)
{
    DBG_Assert(restNode != 0);
    DBG_Assert(!attributes.empty());

    if (!restNode || attributes.empty() || !restNode->isAvailable())
    {
        return false;
    }

    if (taskCountForAddress(restNode->address()) > 0)
    {
        return false;
    }

    TaskItem task;
    task.taskType = TaskReadAttributes;

//    task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    task.req.setDstEndpoint(endpoint);
    task.req.setDstAddressMode(deCONZ::ApsExtAddress);
    task.req.dstAddress() = restNode->address();
    task.req.setClusterId(clusterId);
    task.req.setProfileId(HA_PROFILE_ID);
    task.req.setSrcEndpoint(getSrcEndpoint(restNode, task.req));

    task.zclFrame.setSequenceNumber(zclSeq++);
    task.zclFrame.setCommandId(deCONZ::ZclReadAttributesId);
    task.zclFrame.setFrameControl(deCONZ::ZclFCProfileCommand |
                             deCONZ::ZclFCDirectionClientToServer |
                             deCONZ::ZclFCDisableDefaultResponse);

    DBG_Printf(DBG_INFO_L2, "read attributes of 0x%016llX cluster: 0x%04X: [ ", restNode->address().ext(), clusterId);

    { // payload
        QDataStream stream(&task.zclFrame.payload(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);

        for (uint i = 0; i < attributes.size(); i++)
        {
            stream << attributes[i];
            if (DBG_IsEnabled(DBG_INFO_L2))
            {
                DBG_Printf(DBG_INFO_L2, "0x%04X ", attributes[i]);
            }
        }
    }

    if (DBG_IsEnabled(DBG_INFO_L2))
    {
        DBG_Printf(DBG_INFO_L2, "]\n");
    }

    { // ZCL frame
        QDataStream stream(&task.req.asdu(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        task.zclFrame.writeToStream(stream);
    }

    return addTask(task);
}

/*! Queue reading Group Identifiers.
    \param node the node from which the group identifiers shall be read
    \param startIndex the index to start the reading
    \return true if the request is queued
 */
bool DeRestPluginPrivate::getGroupIdentifiers(RestNodeBase *node, quint8 endpoint, quint8 startIndex)
{
    DBG_Assert(node != 0);

    if (!node || !node->isAvailable())
    {
        return false;
    }

    TaskItem task;
    task.taskType = TaskGetGroupIdentifiers;

    task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    task.req.setDstEndpoint(endpoint);
    task.req.setDstAddressMode(deCONZ::ApsExtAddress);
    task.req.dstAddress() = node->address();
    task.req.setClusterId(COMMISSIONING_CLUSTER_ID);
    task.req.setProfileId(HA_PROFILE_ID); // utility commands (ref.: zll spec. 7.1.1)
    task.req.setSrcEndpoint(getSrcEndpoint(node, task.req));

    task.zclFrame.setSequenceNumber(zclSeq++);
    task.zclFrame.setCommandId(0x41); // get group identifiers cmd
    task.zclFrame.setFrameControl(deCONZ::ZclFCClusterCommand |
                             deCONZ::ZclFCDirectionClientToServer |
                             deCONZ::ZclFCDisableDefaultResponse);

    { // payload
        QDataStream stream(&task.zclFrame.payload(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);

        stream << startIndex;
    }

    { // ZCL frame
        QDataStream stream(&task.req.asdu(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        task.zclFrame.writeToStream(stream);
    }

    DBG_Printf(DBG_INFO, "Send get group identifiers for node 0%04X \n", node->address().ext());

    return addTask(task);
}

/*! Queue writing a ZCL attribute of a node.
    \param restNode the node from which the attributes shall be read
    \param endpoint the destination endpoint
    \param clusterId the cluster id related to the attributes
    \param attribute the attribute to write
    \return true if the request is queued
 */
bool DeRestPluginPrivate::writeAttribute(RestNodeBase *restNode, quint8 endpoint, uint16_t clusterId, const deCONZ::ZclAttribute &attribute)
{
    DBG_Assert(restNode != 0);

    if (!restNode || !restNode->isAvailable())
    {
        return false;
    }

    TaskItem task;
    task.taskType = TaskWriteAttribute;

    task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    task.req.setDstEndpoint(endpoint);
    task.req.setDstAddressMode(deCONZ::ApsExtAddress);
    task.req.dstAddress() = restNode->address();
    task.req.setClusterId(clusterId);
    task.req.setProfileId(HA_PROFILE_ID);
    task.req.setSrcEndpoint(getSrcEndpoint(restNode, task.req));

    task.zclFrame.setSequenceNumber(zclSeq++);
    task.zclFrame.setCommandId(deCONZ::ZclWriteAttributesId);
    task.zclFrame.setFrameControl(deCONZ::ZclFCProfileCommand |
                             deCONZ::ZclFCDirectionClientToServer |
                             deCONZ::ZclFCDisableDefaultResponse);

    { // payload
        QDataStream stream(&task.zclFrame.payload(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);

        stream << attribute.id();
        stream << attribute.dataType();

        if (!attribute.writeToStream(stream))
        {
            return false;
        }
    }

    { // ZCL frame
        QDataStream stream(&task.req.asdu(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        task.zclFrame.writeToStream(stream);
    }

    return addTask(task);
}

/*! Queue reading details of a scene from a node.
    \param restNode the node from which the scene details shall be read
    \param groupId the group Id of the scene
    \param sceneId the scene Id
    \return true if the request is queued
 */
bool DeRestPluginPrivate::readSceneAttributes(LightNode *lightNode, uint16_t groupId, uint8_t sceneId )
{
    DBG_Assert(lightNode != 0);

    if (!lightNode || !lightNode->isAvailable())
    {
        return false;
    }

    TaskItem task;
    task.taskType = TaskViewScene;

//    task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    task.req.setDstEndpoint(lightNode->haEndpoint().endpoint());
    task.req.setDstAddressMode(deCONZ::ApsExtAddress);
    task.req.dstAddress() = lightNode->address();
    task.req.setClusterId(SCENE_CLUSTER_ID);
    task.req.setProfileId(HA_PROFILE_ID);
    task.req.setSrcEndpoint(getSrcEndpoint(lightNode, task.req));

    task.zclFrame.setSequenceNumber(zclSeq++);
    task.zclFrame.setCommandId(0x01); // view scene
    task.zclFrame.setFrameControl(deCONZ::ZclFCClusterCommand |
                             deCONZ::ZclFCDirectionClientToServer |
                             deCONZ::ZclFCDisableDefaultResponse);

    { // payload
        QDataStream stream(&task.zclFrame.payload(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);

        stream << groupId;
        stream << sceneId;
    }

    { // ZCL frame
        QDataStream stream(&task.req.asdu(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        task.zclFrame.writeToStream(stream);
    }

    return addTask(task);
}

/*! Get group membership of a node.
    \param lightNode the node from which the groups shall be discovered
    \param groups - 0 or more group ids
 */
bool DeRestPluginPrivate::readGroupMembership(LightNode *lightNode, const std::vector<uint16_t> &groups)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode || !lightNode->isAvailable())
    {
        return false;
    }

    TaskItem task;
    task.taskType = TaskGetGroupMembership;

//    task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    task.req.setDstEndpoint(lightNode->haEndpoint().endpoint());
    task.req.setDstAddressMode(deCONZ::ApsExtAddress);
    task.req.dstAddress() = lightNode->address();
    task.req.setClusterId(GROUP_CLUSTER_ID);
    task.req.setProfileId(HA_PROFILE_ID);
    task.req.setSrcEndpoint(getSrcEndpoint(lightNode, task.req));

    task.zclFrame.setSequenceNumber(zclSeq++);
    task.zclFrame.setCommandId(0x02); // get group membership
    task.zclFrame.setFrameControl(deCONZ::ZclFCClusterCommand |
                             deCONZ::ZclFCDirectionClientToServer |
                             deCONZ::ZclFCDisableDefaultResponse);

    { // payload
        QDataStream stream(&task.zclFrame.payload(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);

        stream << (uint8_t)groups.size();

        for (uint i = 0; i < groups.size(); i++)
        {
            stream << groups[i];
        }
    }

    { // ZCL frame
        QDataStream stream(&task.req.asdu(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        task.zclFrame.writeToStream(stream);
    }

    return addTask(task);
}

/*! Checks if a group membership is already known.
    If not the group will be added and node gets marked for update.
 */
void DeRestPluginPrivate::foundGroupMembership(LightNode *lightNode, uint16_t groupId)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode)
    {
        return;
    }

    Group *group = getGroupForId(groupId);

    // check if the group is known in the node
    std::vector<GroupInfo>::iterator i = lightNode->groups().begin();
    std::vector<GroupInfo>::iterator end = lightNode->groups().end();

    for (; i != end; ++i)
    {
        if (i->id == groupId)
        {
            if (group && group->state() != Group::StateNormal && group->m_deviceMemberships.size() == 0) // don't touch group of switch
            {
                i->actions &= ~GroupInfo::ActionAddToGroup; // sanity
                i->actions |= GroupInfo::ActionRemoveFromGroup;
                if (i->state != GroupInfo::StateNotInGroup)
                {
                    i->state = GroupInfo::StateNotInGroup;
                    queSaveDb(DB_LIGHTS, DB_SHORT_SAVE_DELAY);
                }
            }

            return; // ok already known
        }
    }

    updateEtag(lightNode->etag);
    updateEtag(gwConfigEtag);

    GroupInfo groupInfo;
    groupInfo.id = groupId;

    if (group)
    {
        updateEtag(group->etag);

        if (group->state() != Group::StateNormal && group->m_deviceMemberships.size() == 0) // don't touch group of switch
        {
            groupInfo.actions &= ~GroupInfo::ActionAddToGroup; // sanity
            groupInfo.actions |= GroupInfo::ActionRemoveFromGroup;
            groupInfo.state = GroupInfo::StateNotInGroup;
        }
        else
        {
            lightNode->enableRead(READ_SCENES); // force reading of scene membership
        }
    }

    queSaveDb(DB_LIGHTS, DB_SHORT_SAVE_DELAY);
    lightNode->groups().push_back(groupInfo);
    markForPushUpdate(lightNode);
}

/*! Checks if the group is known in the global cache.
    If not it will be added.
 */
void DeRestPluginPrivate::foundGroup(uint16_t groupId)
{
    // check if group is known global
    std::vector<Group>::iterator i = groups.begin();
    std::vector<Group>::iterator end = groups.end();

    for (; i != end; ++i)
    {
        if (i->address() == groupId)
        {
            return; // ok already known
        }
    }

    Group group;
    group.setAddress(groupId);
    group.colorX = 0;
    group.colorY = 0;
    group.setIsOn(false);
    group.level = 128;
    group.hue = 0;
    group.hueReal = 0.0f;
    group.sat = 128;
    group.setName(QString());
    updateEtag(group.etag);
    openDb();
    loadGroupFromDb(&group);
    closeDb();
    if (group.name().isEmpty()) {
        group.setName(QString("Group %1").arg(group.id()));
        queSaveDb(DB_GROUPS, DB_SHORT_SAVE_DELAY);
    }
    groups.push_back(group);
    updateEtag(gwConfigEtag);
}

/*! Returns true if the \p lightNode is member of the group with the \p groupId.
 */
bool DeRestPluginPrivate::isLightNodeInGroup(LightNode *lightNode, uint16_t groupId)
{
    DBG_Assert(lightNode != 0);

    if (lightNode)
    {
        std::vector<GroupInfo>::const_iterator i = lightNode->groups().begin();
        std::vector<GroupInfo>::const_iterator end = lightNode->groups().end();

        for (; i != end; ++i)
        {
            if (i->id == groupId && i->state == GroupInfo::StateInGroup)
            {
                return true;
            }
        }
    }

    return false;
}

/*! Delete the light with the \p lightId from all Scenes of the Group with the given \p groupId.
    Also remove these scenes from the Device.
 */
void DeRestPluginPrivate::deleteLightFromScenes(QString lightId, uint16_t groupId)
{
    Group *group = getGroupForId(groupId);
    LightNode *lightNode = getLightNodeForId(lightId);

    if (group)
    {
        std::vector<Scene>::iterator i = group->scenes.begin();
        std::vector<Scene>::iterator end = group->scenes.end();

        for (; i != end; ++i)
        {
            i->deleteLight(lightId);

            // send remove scene request to lightNode
            if (isLightNodeInGroup(lightNode, group->address()))
            {
                GroupInfo *groupInfo = getGroupInfo(lightNode, group->address());

                std::vector<uint8_t> &v = groupInfo->removeScenes;

                if (std::find(v.begin(), v.end(), i->id) == v.end())
                {
                    groupInfo->removeScenes.push_back(i->id);
                }
            }
        }
    }
}

/*! Force reading attributes of all nodes in a group.
 */
void DeRestPluginPrivate::readAllInGroup(Group *group)
{
    DBG_Assert(group != 0);

    if (!group)
    {
        return;
    }

    std::vector<LightNode>::iterator i = nodes.begin();
    std::vector<LightNode>::iterator end = nodes.end();

    for (; i != end; ++i)
    {
        LightNode *lightNode = &(*i);
        if (isLightNodeInGroup(lightNode, group->address()))
        {
            // force reading attributes
            lightNode->setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongerDelay));
            lightNode->enableRead(READ_ON_OFF | READ_COLOR | READ_LEVEL);
        }
    }
}

/*! Set on/off attribute for all nodes in a group.
 */
void DeRestPluginPrivate::setAttributeOnOffGroup(Group *group, uint8_t onOff)
{
    DBG_Assert(group != 0);

    if (!group)
    {
        return;
    }

    bool changed = false;
    bool on = (onOff == 0x01);
    if (on != group->isOn())
    {
        group->setIsOn(on);
        updateEtag(group->etag);
        changed = true;
    }

    std::vector<LightNode>::iterator i = nodes.begin();
    std::vector<LightNode>::iterator end = nodes.end();

    for (; i != end; ++i)
    {
        LightNode *lightNode = &(*i);
        if (isLightNodeInGroup(lightNode, group->address()))
        {
            if (lightNode->isOn() != on)
            {
                lightNode->setIsOn(on);
                updateEtag(lightNode->etag);
                changed = true;
            }
            setAttributeOnOff(lightNode);
        }
    }

    if (changed)
    {
        updateEtag(gwConfigEtag);
    }
}

/*! Get scene membership of a node for a group.
    \param group - the group of interrest
 */
bool DeRestPluginPrivate::readSceneMembership(LightNode *lightNode, Group *group)
{
    DBG_Assert(lightNode != 0);
    DBG_Assert(group != 0);

    if (!lightNode || !group || !lightNode->isAvailable())
    {
        return false;
    }

    TaskItem task;
    task.taskType = TaskGetSceneMembership;

//    task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    task.req.setDstEndpoint(lightNode->haEndpoint().endpoint());
    task.req.setDstAddressMode(deCONZ::ApsExtAddress);
    task.req.dstAddress() = lightNode->address();
    task.req.setClusterId(SCENE_CLUSTER_ID);
    task.req.setProfileId(HA_PROFILE_ID);
    task.req.setSrcEndpoint(getSrcEndpoint(lightNode, task.req));

    task.zclFrame.setSequenceNumber(zclSeq++);
    task.zclFrame.setCommandId(0x06); // get scene membership
    task.zclFrame.setFrameControl(deCONZ::ZclFCClusterCommand |
                             deCONZ::ZclFCDirectionClientToServer |
                             deCONZ::ZclFCDisableDefaultResponse);

    { // payload
        QDataStream stream(&task.zclFrame.payload(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);

        stream << group->address();
    }

    { // ZCL frame
        QDataStream stream(&task.req.asdu(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        task.zclFrame.writeToStream(stream);
    }

    return addTask(task);
}

/*! Checks if the scene membership is known to the group.
    If the scene is not known it will be added.
 */
void DeRestPluginPrivate::foundScene(LightNode *lightNode, Group *group, uint8_t sceneId)
{
    DBG_Assert(group != 0);

    if (!group)
    {
        return;
    }

    std::vector<Scene>::iterator i = group->scenes.begin();
    std::vector<Scene>::iterator end = group->scenes.end();

    for (; i != end; ++i)
    {
        if (i->id == sceneId)
        {
            if (i->state == Scene::StateDeleted && group->m_deviceMemberships.size() == 0) // don't touch scenes from switch
            {
                GroupInfo *groupInfo = getGroupInfo(lightNode, group->address());

                if (groupInfo)
                {
                    std::vector<uint8_t> &v = groupInfo->removeScenes;

                    if (std::find(v.begin(), v.end(), sceneId) == v.end())
                    {
                        DBG_Printf(DBG_INFO, "Found Scene %u which was deleted before, delete again\n", sceneId);
                        groupInfo->removeScenes.push_back(sceneId);
                    }
                }
            }
            return; // already known
        }
    }

    Scene scene;
    scene.groupAddress = group->address();
    scene.id = sceneId;
    openDb();
    loadSceneFromDb(&scene);
    closeDb();
    if (scene.name.isEmpty())
    {
        scene.name.sprintf("Scene %u", sceneId);
    }
    group->scenes.push_back(scene);
    updateEtag(group->etag);
    updateEtag(gwConfigEtag);
    queSaveDb(DB_SCENES, DB_SHORT_SAVE_DELAY);
}

/*! Sets the name of a scene which will be saved in the database.
 */
void DeRestPluginPrivate::setSceneName(Group *group, uint8_t sceneId, const QString &name)
{
    DBG_Assert(group != 0);
    DBG_Assert(name.size() != 0);

    if(!group || name.isEmpty())
    {
        return;
    }

    std::vector<Scene>::iterator i = group->scenes.begin();
    std::vector<Scene>::iterator end = group->scenes.end();

    for (; i != end; ++i)
    {
        if (i->id == sceneId)
        {
            i->name = name;
            queSaveDb(DB_SCENES, DB_SHORT_SAVE_DELAY);
            updateEtag(group->etag);
            break;
        }
    }
}

/*! Sends a store scene request to a group.
 */
bool DeRestPluginPrivate::storeScene(Group *group, uint8_t sceneId)
{
    DBG_Assert(group != 0);

    if (!group)
    {
        return false;
    }

    std::vector<LightNode>::iterator i = nodes.begin();
    std::vector<LightNode>::iterator end = nodes.end();
    for (; i != end; ++i)
    {
        LightNode *lightNode = &(*i);
        if (lightNode->isAvailable() && // note: we only create/store the scene if node is available
            isLightNodeInGroup(lightNode, group->address()) )
        {
            GroupInfo *groupInfo = getGroupInfo(lightNode, group->address());

            if (lightNode->sceneCapacity() != 0 || groupInfo->sceneCount() != 0) //xxx workaround
            {
                std::vector<uint8_t> &v = groupInfo->addScenes;

                if (std::find(v.begin(), v.end(), sceneId) == v.end())
                {
                    groupInfo->addScenes.push_back(sceneId);
                }
            }
        }
    }

    return true;
}

/*! Sends a modify scene request to a group.
 */
bool DeRestPluginPrivate::modifyScene(Group *group, uint8_t sceneId)
{
    DBG_Assert(group != 0);

    if (!group)
    {
        return false;
    }

    std::vector<LightNode>::iterator i = nodes.begin();
    std::vector<LightNode>::iterator end = nodes.end();
    for (; i != end; ++i)
    {
        LightNode *lightNode = &(*i);
        if (lightNode->isAvailable() && // note: we only modify the scene if node is available
            isLightNodeInGroup(lightNode, group->address()))
        {
            GroupInfo *groupInfo = getGroupInfo(lightNode, group->address());

            std::vector<uint8_t> &v = groupInfo->modifyScenes;

            if (std::find(v.begin(), v.end(), sceneId) == v.end())
            {
                groupInfo->modifyScenes.push_back(sceneId);
            }
        }
    }

    return true;
}

/*! Sends a remove scene request to a group.
 */
bool DeRestPluginPrivate::removeScene(Group *group, uint8_t sceneId)
{
    DBG_Assert(group != 0);

    if (!group)
    {
        return false;
    }

    {
        std::vector<Scene>::iterator i = group->scenes.begin();
        std::vector<Scene>::iterator end = group->scenes.end();

        for (; i != end; ++i)
        {
            if (i->id == sceneId)
            {
                i->state = Scene::StateDeleted;
                updateEtag(group->etag);
                updateEtag(gwConfigEtag);
                break;
            }
        }
    }

    std::vector<LightNode>::iterator i = nodes.begin();
    std::vector<LightNode>::iterator end = nodes.end();
    for (; i != end; ++i)
    {
        LightNode *lightNode = &(*i);
        // note: we queue removing of scene even if node is not available
        if (isLightNodeInGroup(lightNode, group->address()))
        {
            GroupInfo *groupInfo = getGroupInfo(lightNode, group->address());

            std::vector<uint8_t> &v = groupInfo->removeScenes;

            if (std::find(v.begin(), v.end(), sceneId) == v.end())
            {
                groupInfo->removeScenes.push_back(sceneId);
            }
        }
    }

    return true;
}

/*! Sends a call scene request to a group.
 */
bool DeRestPluginPrivate::callScene(Group *group, uint8_t sceneId)
{
    DBG_Assert(group != 0);

    if (!group)
    {
        return false;
    }

    TaskItem task;
    task.taskType = TaskCallScene;

    task.req.setTxOptions(0);
    task.req.setDstEndpoint(0xFF);
    task.req.setDstAddressMode(deCONZ::ApsGroupAddress);
    task.req.dstAddress().setGroup(group->address());
    task.req.setClusterId(SCENE_CLUSTER_ID);
    task.req.setProfileId(HA_PROFILE_ID);
    task.req.setSrcEndpoint(getSrcEndpoint(0, task.req));

    task.zclFrame.setSequenceNumber(zclSeq++);
    task.zclFrame.setCommandId(0x05); // recall scene
    task.zclFrame.setFrameControl(deCONZ::ZclFCClusterCommand |
                             deCONZ::ZclFCDirectionClientToServer |
                             deCONZ::ZclFCDisableDefaultResponse);

    { // payload
        QDataStream stream(&task.zclFrame.payload(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);

        stream << group->address();
        stream << sceneId;
    }

    { // ZCL frame
        QDataStream stream(&task.req.asdu(), QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        task.zclFrame.writeToStream(stream);
    }

    if (addTask(task))
    {
        return true;
    }

    return false;
}

/*! Queues a client for closing the connection.
    \param sock the client socket
    \param closeTimeout timeout in seconds then the socket should be closed
 */
void DeRestPluginPrivate::pushClientForClose(QTcpSocket *sock, int closeTimeout)
{
    std::list<TcpClient>::iterator i = openClients.begin();
    std::list<TcpClient>::iterator end = openClients.begin();

    for ( ;i != end; ++i)
    {
        if (i->sock == sock)
        {
            i->closeTimeout = closeTimeout;
            return;
        }
        // Other QtcpSocket but same peer
        else if (i->sock->peerPort() == sock->peerPort())
        {
            if (i->sock->peerAddress() == sock->peerAddress())
            {
                i->sock->deleteLater();

                i->sock = sock;
                i->closeTimeout = closeTimeout;
                return;
            }
        }
    }

    TcpClient client;
    client.sock = sock;
    client.closeTimeout = closeTimeout;

    openClients.push_back(client);
}

/*! Adds a task to the queue.
    \return true - on success
 */
bool DeRestPluginPrivate::addTask(const TaskItem &task)
{
    if (!isInNetwork())
    {
        return false;
    }

    const uint MaxTasks = 20;

    std::list<TaskItem>::iterator i = tasks.begin();
    std::list<TaskItem>::iterator end = tasks.end();

    if ((task.taskType != TaskGetSceneMembership) &&
        (task.taskType != TaskGetGroupMembership) &&
        (task.taskType != TaskGetGroupIdentifiers) &&
        (task.taskType != TaskStoreScene) &&
        (task.taskType != TaskRemoveScene) &&
        (task.taskType != TaskRemoveAllScenes) &&
        (task.taskType != TaskReadAttributes) &&
        (task.taskType != TaskWriteAttribute) &&
        (task.taskType != TaskViewScene) &&
        (task.taskType != TaskAddScene))
    {
        for (; i != end; ++i)
        {
            if (i->taskType == task.taskType)
            {
                if ((i->req.dstAddress() ==  task.req.dstAddress()) &&
                    (i->req.dstEndpoint() ==  task.req.dstEndpoint()) &&
                    (i->req.srcEndpoint() ==  task.req.srcEndpoint()) &&
                    (i->req.profileId() ==  task.req.profileId()) &&
                    (i->req.clusterId() ==  task.req.clusterId()) &&
                    (i->req.txOptions() ==  task.req.txOptions()) &&
                    (i->req.asdu().size() ==  task.req.asdu().size()))

                {
                    DBG_Printf(DBG_INFO, "Replace task in queue cluster 0x%04X with newer task of same type\n", task.req.clusterId());
                    *i = task;
                    return true;
                }
            }
        }
    }

    if (tasks.size() < MaxTasks) {
        tasks.push_back(task);
        return true;
    }

    return false;
}

/*! Fills cluster, lightNode and node fields of \p task based on the information in \p ind.
    \return true - on success
 */
bool DeRestPluginPrivate::obtainTaskCluster(TaskItem &task, const deCONZ::ApsDataIndication &ind)
{
    deCONZ::SimpleDescriptor *sd = 0;

    task.node = 0;
    task.lightNode = 0;
    task.cluster = 0;

    if (task.req.dstAddressMode() == deCONZ::ApsExtAddress)
    {
        quint64 extAddr = task.req.dstAddress().ext();

        task.lightNode = getLightNodeForAddress(extAddr, task.req.dstEndpoint());
        task.node = getNodeForAddress(extAddr);

        if (!task.node)
        {
            return false;
        }

        sd = task.node->getSimpleDescriptor(task.req.dstEndpoint());
        if (!sd)
        {
            return false;
        }

        task.cluster = sd->cluster(ind.clusterId(), deCONZ::ServerCluster);
    }
    else
    {
        // broadcast not supported
        return false;
    }

    if (!task.lightNode || !task.node || !task.cluster)
    {
        return false;
    }

    return true;
}

/*! Fires the next APS-DATA.request.
 */
void DeRestPluginPrivate::processTasks()
{
    if (!apsCtrl)
    {
        return;
    }

    if (tasks.empty())
    {
        return;
    }

    if (!isInNetwork())
    {
        DBG_Printf(DBG_INFO, "Not in network cleanup %d tasks\n", (runningTasks.size() + tasks.size()));
        runningTasks.clear();
        tasks.clear();
        return;
    }

    if (runningTasks.size() > 4)
    {
        DBG_Printf(DBG_INFO, "%d running tasks, wait\n", runningTasks.size());
        return;
    }

    std::list<TaskItem>::iterator i = tasks.begin();
    std::list<TaskItem>::iterator end = tasks.end();

    for (; i != end; ++i)
    {
        // drop dead unicasts
        if (i->lightNode && !i->lightNode->isAvailable())
        {
            DBG_Printf(DBG_INFO, "drop request to zombie\n");
            tasks.erase(i);
            return;
        }

        // send only one request to a destination at a time
        std::list<TaskItem>::iterator j = runningTasks.begin();
        std::list<TaskItem>::iterator jend = runningTasks.end();

        bool ok = true;
        for (; j != jend; ++j)
        {
            if (i->req.dstAddress() == j->req.dstAddress())
            {
                ok = false;
                break;
            }
        }

        if (!ok) // destination already busy
        {
            if (i->req.dstAddressMode() == deCONZ::ApsExtAddress)
            {
                DBG_Printf(DBG_INFO_L2, "delay sending request %u to %s\n", i->req.id(), qPrintable(i->req.dstAddress().toStringExt()));
            }
            else if (i->req.dstAddressMode() == deCONZ::ApsGroupAddress)
            {
                DBG_Printf(DBG_INFO, "delay sending request %u to group 0x%04X\n", i->req.id(), i->req.dstAddress().group());
            }
        }
        else
        {
            bool pushRunning = (i->req.state() != deCONZ::FireAndForgetState);

            // groupcast tasks
            if (i->req.dstAddressMode() == deCONZ::ApsGroupAddress)
            {
                Group *group = getGroupForId(i->req.dstAddress().group());

                if (group)
                {
                    QTime now = QTime::currentTime();
                    int diff = group->sendTime.msecsTo(now);

                    if (!group->sendTime.isValid() || (diff <= 0) || (diff > gwGroupSendDelay))
                    {
                        if (apsCtrl->apsdeDataRequest(i->req) == deCONZ::Success)
                        {
                            group->sendTime = now;
                            if (pushRunning)
                            {
                                runningTasks.push_back(*i);
                            }
                            tasks.erase(i);
                            return;
                        }
                    }
                    else
                    {
                        DBG_Printf(DBG_INFO, "delayed group sending\n");
                    }
                }
            }
            // unicast/broadcast tasks
            else
            {
                if (i->lightNode && !i->lightNode->isAvailable())
                {
                    DBG_Printf(DBG_INFO, "drop request to zombie\n");
                    tasks.erase(i);
                    return;
                }
                else
                {
                    int ret = apsCtrl->apsdeDataRequest(i->req);

                    if (ret == deCONZ::Success)
                    {
                        if (pushRunning)
                        {
                            runningTasks.push_back(*i);
                        }
                        tasks.erase(i);
                        return;
                    }
                    else if (ret == deCONZ::ErrorNodeIsZombie)
                    {
                        DBG_Printf(DBG_INFO, "drop request to zombie\n");
                        tasks.erase(i);
                        return;
                    }
                    else
                    {
                        DBG_Printf(DBG_INFO, "enqueue APS request failed with error %d\n", ret);
                    }
                }
            }
        }
    }
}

/*! Handler for node events.
    \param event the event which occured
 */
void DeRestPluginPrivate::nodeEvent(const deCONZ::NodeEvent &event)
{
    if (event.event() != deCONZ::NodeEvent::NodeDeselected)
    {
        if (!event.node())
        {
            return;
        }
    }

    switch (event.event())
    {
    case deCONZ::NodeEvent::NodeSelected:
        break;

    case deCONZ::NodeEvent::NodeDeselected:
        break;

    case deCONZ::NodeEvent::NodeRemoved:
    {
        std::vector<LightNode>::iterator i = nodes.begin();
        std::vector<LightNode>::iterator end = nodes.end();

        for (; i != end; ++i)
        {
            if (i->address().ext() == event.node()->address().ext())
            {
                DBG_Printf(DBG_INFO, "LightNode removed %s\n", qPrintable(event.node()->address().toStringExt()));
                i->setIsAvailable(false);
                updateEtag(i->etag);
                updateEtag(gwConfigEtag);
            }
        }
    }
        break;

    case deCONZ::NodeEvent::NodeAdded:
    {
        addLightNode(event.node());
        addSensorNode(event.node());
    }
        break;

    case deCONZ::NodeEvent::NodeZombieChanged:
    {
        DBG_Printf(DBG_INFO, "Node zombie state changed %s\n", qPrintable(event.node()->address().toStringExt()));
        nodeZombieStateChanged(event.node());
    }
        break;

    case deCONZ::NodeEvent::UpdatedSimpleDescriptor:
    {
        addLightNode(event.node());
        addSensorNode(event.node());
    }
        break;

    case deCONZ::NodeEvent::UpdatedPowerDescriptor:
    {
        updateSensorNode(event);
    }
        break;

    case deCONZ::NodeEvent::UpdatedClusterData:
    case deCONZ::NodeEvent::UpdatedClusterDataZclRead:
    case deCONZ::NodeEvent::UpdatedClusterDataZclReport:
    {
        if (event.profileId() == ZDP_PROFILE_ID && event.clusterId() == ZDP_ACTIVE_ENDPOINTS_RSP_CLID)
        {
            updateSensorNode(event);
            return;
        }

        if (event.profileId() != HA_PROFILE_ID && event.profileId() != ZLL_PROFILE_ID)
        {
            return;
        }

        DBG_Printf(DBG_INFO_L2, "Node data %s profileId: 0x%04X, clusterId: 0x%04X\n", qPrintable(event.node()->address().toStringExt()), event.profileId(), event.clusterId());

        // filter for supported sensor clusters
        switch (event.clusterId())
        {
        // sensor node?
        case ONOFF_SWITCH_CONFIGURATION_CLUSTER_ID:
        case ILLUMINANCE_MEASUREMENT_CLUSTER_ID:
        case ILLUMINANCE_LEVEL_SENSING_CLUSTER_ID:
        case OCCUPANCY_SENSING_CLUSTER_ID:
        case BASIC_CLUSTER_ID:
            {
                updateSensorNode(event);
            }
            break;

        default:
            break;
        }

        // filter for supported light clusters
        switch (event.clusterId())
        {
        // sensor node?
        case BASIC_CLUSTER_ID:
        case IDENTIFY_CLUSTER_ID:
        case ONOFF_CLUSTER_ID:
        case LEVEL_CLUSTER_ID:
        case GROUP_CLUSTER_ID:
        case SCENE_CLUSTER_ID:
        case COLOR_CLUSTER_ID:
            {
                updateLightNode(event);
            }
            break;

        default:
            break;
        }
    }
        break;

    default:
        break;
    }
}

/*! Process task like add to group and remove from group.
 */
void DeRestPluginPrivate::processGroupTasks()
{
    if (nodes.empty())
    {
        return;
    }

    if (!isInNetwork())
    {
        return;
    }

    if (tasks.size() > MaxGroupTasks)
    {
        return;
    }

    if (groupTaskNodeIter >= nodes.size())
    {
        groupTaskNodeIter = 0;
    }

    TaskItem task;

    task.lightNode = &nodes[groupTaskNodeIter];
    groupTaskNodeIter++;

    if (!task.lightNode->isAvailable())
    {
        return;
    }

    // set destination parameters
    task.req.dstAddress() = task.lightNode->address();
//    task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    task.req.setDstEndpoint(task.lightNode->haEndpoint().endpoint());
    task.req.setSrcEndpoint(getSrcEndpoint(task.lightNode, task.req));
    task.req.setDstAddressMode(deCONZ::ApsExtAddress);

    std::vector<GroupInfo>::iterator i = task.lightNode->groups().begin();
    std::vector<GroupInfo>::iterator end = task.lightNode->groups().end();

    for (; i != end; ++i)
    {
        if (i->actions & GroupInfo::ActionAddToGroup)
        {
            if (addTaskAddToGroup(task, i->id))
            {
                i->actions &= ~GroupInfo::ActionAddToGroup;
            }
            return;
        }

        if (i->actions & GroupInfo::ActionRemoveFromGroup)
        {
            if (addTaskRemoveFromGroup(task, i->id))
            {
                i->actions &= ~GroupInfo::ActionRemoveFromGroup;
            }
            return;
        }

        if (!i->addScenes.empty())
        {
            if (addTaskStoreScene(task, i->id, i->addScenes[0]))
            {
                processTasks();
                return;
            }
        }

        if (!i->removeScenes.empty())
        {
            if (addTaskRemoveScene(task, i->id, i->removeScenes[0]))
            {
                processTasks();
                return;
            }
        }

        if (!i->modifyScenes.empty())
        {
            if (addTaskAddScene(task, i->id, i->modifyScenes[0], task.lightNode->id()))
            {
                processTasks();
                return;
            }
        }
    }
}

/*! Handle packets related to the ZCL group cluster.
    \param task the task which belongs to this response
    \param ind the APS level data indication containing the ZCL packet
    \param zclFrame the actual ZCL frame which holds the groups cluster reponse
 */
void DeRestPluginPrivate::handleGroupClusterIndication(TaskItem &task, const deCONZ::ApsDataIndication &ind, deCONZ::ZclFrame &zclFrame)
{
    Q_UNUSED(task);

    if (!ind.srcAddress().hasExt())
    {
        return;
    }

    LightNode *lightNode = getLightNodeForAddress(ind.srcAddress().ext(), ind.srcEndpoint());
    int endpointCount = getNumberOfEndpoints(ind.srcAddress().ext());

    if (!lightNode)
    {
        return;
    }

    if (zclFrame.isDefaultResponse())
    {
    }
    else if (zclFrame.commandId() == 0x02) // Get group membership response
    {
        DBG_Assert(zclFrame.payload().size() >= 2);

        QDataStream stream(zclFrame.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        uint8_t capacity;
        uint8_t count;

        stream >> capacity;
        stream >> count;

        lightNode->setGroupCapacity(capacity);
        lightNode->setGroupCount(count);

        DBG_Printf(DBG_INFO, "verified group capacity: %u and group count: %u of LightNode %s\n", capacity, count, qPrintable(lightNode->address().toStringExt()));

        QVector<quint16> responseGroups;
        for (uint i = 0; i < count; i++)
        {
            if (!stream.atEnd())
            {
                uint16_t groupId;
                stream >> groupId;

                responseGroups.push_back(groupId);

                DBG_Printf(DBG_INFO, "%s found group 0x%04X\n", qPrintable(lightNode->address().toStringExt()), groupId);

                foundGroup(groupId);
                foundGroupMembership(lightNode, groupId);
            }
        }

        std::vector<GroupInfo>::iterator i = lightNode->groups().begin();
        std::vector<GroupInfo>::iterator end = lightNode->groups().end();

        for (; i != end; ++i)
        {
            Group *group = getGroupForId(i->id);

            if (group && group->state() == Group::StateNormal
                && group->m_deviceMemberships.size() == 0 //no switch group
                && !responseGroups.contains(i->id)
                && i->state == GroupInfo::StateInGroup)
            {
                    DBG_Printf(DBG_INFO, "restore group  0x%04X for lightNode %s\n", i->id, qPrintable(lightNode->address().toStringExt()));
                    i->actions &= ~GroupInfo::ActionRemoveFromGroup; // sanity
                    i->actions |= GroupInfo::ActionAddToGroup;
                    i->state = GroupInfo::StateInGroup;
                    updateEtag(group->etag);
                    updateEtag(gwConfigEtag);
                    queSaveDb(DB_LIGHTS, DB_SHORT_SAVE_DELAY);
            }
            else if (group && group->state() == Group::StateNormal
                && group->m_deviceMemberships.size() > 0) //a switch group
            {
                if (responseGroups.contains(i->id)
                    && i->state == GroupInfo::StateNotInGroup) // light was added by a switch -> add it to deCONZ group)
                {
                    i->state = GroupInfo::StateInGroup;
                    std::vector<QString> &v = group->m_multiDeviceIds;
                    std::vector<QString>::iterator fi = std::find(v.begin(), v.end(), lightNode->id());
                    if (fi != v.end())
                    {
                        group->m_multiDeviceIds.erase(fi);
                        queSaveDb(DB_GROUPS, DB_SHORT_SAVE_DELAY);
                    }
                    updateEtag(group->etag);
                    updateEtag(gwConfigEtag);
                    queSaveDb(DB_LIGHTS, DB_SHORT_SAVE_DELAY);
                }
                else if (!responseGroups.contains(i->id)
                    && i->state == GroupInfo::StateInGroup) // light was removed from group by switch -> remove it from deCONZ group)
                {
                    i->state = GroupInfo::StateNotInGroup;
                    updateEtag(group->etag);
                    updateEtag(gwConfigEtag);
                    queSaveDb(DB_LIGHTS, DB_SHORT_SAVE_DELAY);
                }
            }
        }
    }
    else if (zclFrame.commandId() == 0x00) // Add group response
    {
        DBG_Assert(zclFrame.payload().size() >= 2);

        QDataStream stream(zclFrame.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        uint8_t status;
        uint16_t groupId;

        stream >> status;
        stream >> groupId;

        if (status == 0x00)
        {
            uint8_t capacity = lightNode->groupCapacity();
            if (capacity >= endpointCount)
            {
                capacity = capacity - endpointCount;
            }
            lightNode->setGroupCapacity(capacity);

            uint8_t count = lightNode->groupCount();
            if (count < 255)
            {
                count++;
            }
            lightNode->setGroupCount(count);
        }

        DBG_Printf(DBG_INFO, "Add to group response for light %s. Status:0x%02X, capacity: %u\n", qPrintable(lightNode->id()), status, lightNode->groupCapacity());

    }
    else if (zclFrame.commandId() == 0x03) // Remove group response
    {
        DBG_Assert(zclFrame.payload().size() >= 2);

        QDataStream stream(zclFrame.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        uint8_t status;
        uint16_t groupId;

        stream >> status;
        stream >> groupId;

        if (status == 0x00)
        {
            GroupInfo *groupInfo = getGroupInfo(lightNode, groupId);
            DBG_Assert(groupInfo != 0);

            if (groupInfo)
            {
                uint8_t sceneCount = groupInfo->sceneCount();
                uint8_t sceneCapacity = lightNode->sceneCapacity();

                if ((sceneCapacity + sceneCount) <= 255)
                {
                    sceneCapacity = sceneCapacity + sceneCount;
                }
                else
                {
                    sceneCapacity = 255;
                }
                lightNode->setSceneCapacity(sceneCapacity);

                uint8_t capacity = lightNode->groupCapacity();
                if ((capacity + endpointCount) <= 255)
                {
                    capacity = capacity + endpointCount;
                }
                lightNode->setGroupCapacity(capacity);

                uint8_t count = lightNode->groupCount();
                if (count > 0)
                {
                    count--;
                }
                lightNode->setGroupCount(count);
            }
        }

        DBG_Printf(DBG_INFO, "Remove from group response for light %s. Status: 0x%02X, capacity: %u\n", qPrintable(lightNode->id()), status, lightNode->groupCapacity());
    }
}

/*! Handle packets related to the ZCL scene cluster.
    \param task the task which belongs to this response
    \param ind the APS level data indication containing the ZCL packet
    \param zclFrame the actual ZCL frame which holds the scene cluster reponse
 */
void DeRestPluginPrivate::handleSceneClusterIndication(TaskItem &task, const deCONZ::ApsDataIndication &ind, deCONZ::ZclFrame &zclFrame)
{
    Q_UNUSED(task);

    if (zclFrame.isDefaultResponse())
    {
    }
    else if (zclFrame.commandId() == 0x06) // Get scene membership response
    {
        DBG_Assert(zclFrame.payload().size() >= 4);

        QDataStream stream(zclFrame.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        uint8_t status;
        uint8_t capacity;
        uint16_t groupId;
        uint8_t count;

        stream >> status;
        stream >> capacity;
        stream >> groupId;

        if (status == deCONZ::ZclSuccessStatus)
        {
            Group *group = getGroupForId(groupId);
            LightNode *lightNode = getLightNodeForAddress(ind.srcAddress().ext(), ind.srcEndpoint());
            GroupInfo *groupInfo = getGroupInfo(lightNode, group->address());

            stream >> count;

            if (group && lightNode && groupInfo)
            {
                lightNode->setSceneCapacity(capacity);
                groupInfo->setSceneCount(count);

                for (uint i = 0; i < count; i++)
                {
                    if (!stream.atEnd())
                    {
                        uint8_t sceneId;
                        stream >> sceneId;

                        DBG_Printf(DBG_INFO, "found scene 0x%02X for group 0x%04X\n", sceneId, groupId);

                        if (group && lightNode)
                        {
                            foundScene(lightNode, group, sceneId);
                        }
                    }
                }

                lightNode->enableRead(READ_SCENE_DETAILS);
            }
            Q_Q(DeRestPlugin);
            q->startZclAttributeTimer(checkZclAttributesDelay);
        }
    }
    else if (zclFrame.commandId() == 0x04) // Store scene response
    {
        DBG_Assert(zclFrame.payload().size() >= 3);

        QDataStream stream(zclFrame.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        uint8_t status;
        uint16_t groupId;
        uint8_t sceneId;

        stream >> status;
        stream >> groupId;
        stream >> sceneId;

        LightNode *lightNode = getLightNodeForAddress(ind.srcAddress().ext(), ind.srcEndpoint());

        if (lightNode)
        {
            GroupInfo *groupInfo = getGroupInfo(lightNode, groupId);

            if (groupInfo)
            {
                std::vector<uint8_t> &v = groupInfo->addScenes;
                std::vector<uint8_t>::iterator i = std::find(v.begin(), v.end(), sceneId);

                if (i != v.end())
                {
                    DBG_Printf(DBG_INFO, "Added/stored scene %u in node %s Response. Status: 0x%02X\n", sceneId, qPrintable(lightNode->id()), status);
                    groupInfo->addScenes.erase(i);

                    if (status == 0x00)
                    {
                        Scene *scene = getSceneForId(groupId, sceneId);

                        if (scene)
                        {
                            bool foundLightstate = false;

                            std::vector<LightState>::iterator li = scene->lights().begin();
                            std::vector<LightState>::iterator lend = scene->lights().end();
                            for (; li != lend; ++li)
                            {
                                if (li->lid() == lightNode->id())
                                {
                                    li->setOn(lightNode->isOn());
                                    li->setBri((uint8_t)lightNode->level());
                                    li->setX(lightNode->colorX());
                                    li->setY(lightNode->colorY());
                                    li->setColorloopActive(lightNode->isColorLoopActive());
                                    li->setColorloopTime(lightNode->colorLoopSpeed());
                                    foundLightstate = true;
                                    break;
                                }
                            }

                            if (!foundLightstate)
                            {
                                LightState state;
                                state.setLid(lightNode->id());
                                state.setOn(lightNode->isOn());
                                state.setBri((uint8_t)lightNode->level());
                                state.setX(lightNode->colorX());
                                state.setY(lightNode->colorY());
                                state.setColorloopActive(lightNode->isColorLoopActive());
                                state.setColorloopTime(lightNode->colorLoopSpeed());
                                scene->addLight(state);

                                // only change capacity and count when creating a new scene
                                uint8_t sceneCapacity = lightNode->sceneCapacity();
                                if (sceneCapacity > 0)
                                {
                                    sceneCapacity--;
                                }
                                lightNode->setSceneCapacity(sceneCapacity);

                                uint8_t sceneCount = groupInfo->sceneCount();
                                if (sceneCount < 255)
                                {
                                    sceneCount++;
                                }
                                groupInfo->setSceneCount(sceneCount);

                                DBG_Printf(DBG_INFO, "scene capacity: %u\n", sceneCapacity);
                            }

                            queSaveDb(DB_SCENES, DB_SHORT_SAVE_DELAY);
                        }
                    }
                }
            }
        }
    }
    else if (zclFrame.commandId() == 0x02) // Remove scene response
    {
        DBG_Assert(zclFrame.payload().size() >= 4);

        QDataStream stream(zclFrame.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        uint8_t status;
        uint16_t groupId;
        uint8_t sceneId;

        stream >> status;
        stream >> groupId;
        stream >> sceneId;

        LightNode *lightNode = getLightNodeForAddress(ind.srcAddress().ext(), ind.srcEndpoint());

        if (lightNode)
        {
            GroupInfo *groupInfo = getGroupInfo(lightNode, groupId);

            if (groupInfo)
            {
                std::vector<uint8_t> &v = groupInfo->removeScenes;
                std::vector<uint8_t>::iterator i = std::find(v.begin(), v.end(), sceneId);

                if (i != v.end())
                {
                    DBG_Printf(DBG_INFO, "Removed scene %u from node %s status 0x%02X\n", sceneId, qPrintable(lightNode->id()), status);
                    groupInfo->removeScenes.erase(i);

                    if (status == 0x00)
                    {
                        Scene *scene = getSceneForId(groupId, sceneId);

                        if (scene)
                        {
                            std::vector<LightState>::const_iterator li = scene->lights().begin();
                            std::vector<LightState>::const_iterator lend = scene->lights().end();
                            for (; li != lend; ++li)
                            {
                                if (li->lid() == lightNode->id())
                                {
                                    scene->deleteLight(lightNode->id());
                                    break;
                                }
                            }

                            uint8_t sceneCapacity = lightNode->sceneCapacity();
                            if (sceneCapacity < 255)
                            {
                                sceneCapacity++;
                            }
                            lightNode->setSceneCapacity(sceneCapacity);

                            uint8_t sceneCount = groupInfo->sceneCount();
                            if (sceneCount > 0)
                            {
                                sceneCount--;
                            }
                            groupInfo->setSceneCount(sceneCount);

                            DBG_Printf(DBG_INFO, "scene capacity: %u\n", sceneCapacity);
                        }
                    }
                }
            }
        }
    }
    else if (zclFrame.commandId() == 0x00) // Add scene response // will only be created by modifying scene, yet.
    {
        DBG_Assert(zclFrame.payload().size() >= 4);

        QDataStream stream(zclFrame.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        uint8_t status;
        uint16_t groupId;
        uint8_t sceneId;

        stream >> status;
        stream >> groupId;
        stream >> sceneId;

        LightNode *lightNode = getLightNodeForAddress(ind.srcAddress().ext(), ind.srcEndpoint());

        if (lightNode)
        {
            GroupInfo *groupInfo = getGroupInfo(lightNode, groupId);

            if (groupInfo)
            {
                std::vector<uint8_t> &v = groupInfo->modifyScenes;
                std::vector<uint8_t>::iterator i = std::find(v.begin(), v.end(), sceneId);

                if (i != v.end())
                {
                    DBG_Printf(DBG_INFO, "Modified scene %u in node %s status 0x%02X\n", sceneId, qPrintable(lightNode->address().toStringExt()), status);
                    groupInfo->modifyScenes.erase(i);
                }
            }
        }
    }
    else if (zclFrame.commandId() == 0x01) // View scene response
    {
        DBG_Assert(zclFrame.payload().size() >= 4);

        LightNode *lightNode = getLightNodeForAddress(ind.srcAddress().ext(), ind.srcEndpoint());

        QDataStream stream(zclFrame.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        uint8_t status;
        uint16_t groupId;
        uint8_t sceneId;
        uint16_t transitiontime;
        uint8_t length;
        QString sceneName = "";
        LightState light;

        light.setLid(lightNode->id());

        stream >> status;
        if (status == 0x00)
        {
            stream >> groupId;
            stream >> sceneId;
            stream >> transitiontime;
            stream >> length;

            light.setTransitiontime(transitiontime*10);

            for (int i = 0; i < length; i++)
            {
                char *c;
                stream >> c;
                sceneName.append(c);
            }

            while (!stream.atEnd())
            {
                uint16_t clusterId;
                uint8_t l;
                uint8_t fs8;
                uint16_t fs16;

                stream >> clusterId;
                stream >> l;

                if (clusterId == 0x0006)
                {
                    stream >> fs8;
                    bool on = (fs8 == 0x01) ? true : false;
                    light.setOn(on);
                }
                else if (clusterId == 0x0008)
                {
                    stream >> fs8;
                    light.setBri(fs8);
                }
                else if (clusterId == 0x0300)
                {
                    stream >> fs16;
                    light.setX(fs16);

                    stream >> fs16;
                    light.setY(fs16);
                }
            }

            DBG_Printf(DBG_INFO_L2, "Validaded Scene (gid: %u, sid: %u) for Light %s\n", groupId, sceneId, qPrintable(lightNode->id()));
            DBG_Printf(DBG_INFO_L2, "On: %u, Bri: %u, X: %u, Y: %u, Transitiontime: %u\n",
                    light.on(), light.bri(), light.x(), light.y(), light.transitiontime());
        }
    }
    else if (zclFrame.commandId() == 0x05) // Recall scene command
    {
        if (!ind.srcAddress().hasExt())
        {
            return;
        }

        // update Nodes and Groups state if Recall scene Command was send by a switch
        Sensor *sensorNode = getSensorNodeForAddressAndEndpoint(ind.srcAddress().ext(), ind.srcEndpoint());

        if (sensorNode)
        {
            if (sensorNode->deletedState() != Sensor::StateDeleted)
            {
                DBG_Assert(zclFrame.payload().size() >= 3);

                QDataStream stream(zclFrame.payload());
                stream.setByteOrder(QDataStream::LittleEndian);

                uint16_t groupId;
                uint8_t sceneId;

                //stream >> status;
                stream >> groupId;
                stream >> sceneId;

                // check if scene exists
                Scene scene;
                TaskItem task2;
                bool colorloopDeactivated = false;
                Group *group = getGroupForId(groupId);

                if (group && group->state() != Group::StateDeleted && group->state() != Group::StateDeleteFromDB)
                {
                    std::vector<Scene>::const_iterator i = group->scenes.begin();
                    std::vector<Scene>::const_iterator end = group->scenes.end();

                    for (; i != end; ++i)
                    {
                        if ((i->id == sceneId) && (i->state != Scene::StateDeleted))
                        {
                            scene = *i;

                            std::vector<LightState>::const_iterator ls = scene.lights().begin();
                            std::vector<LightState>::const_iterator lsend = scene.lights().end();

                            for (; ls != lsend; ++ls)
                            {
                                LightNode *light = getLightNodeForId(ls->lid());
                                if (light && light->isAvailable() && light->state() != LightNode::StateDeleted)
                                {
                                    bool changed = false;
                                    if (!ls->colorloopActive() && light->isColorLoopActive() != ls->colorloopActive())
                                    {
                                        //stop colorloop if scene was saved without colorloop (Osram don't stop colorloop if another scene is called)
                                        task2.lightNode = light;
                                        task2.req.dstAddress() = task2.lightNode->address();
                                        task2.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
                                        task2.req.setDstEndpoint(task2.lightNode->haEndpoint().endpoint());
                                        task2.req.setSrcEndpoint(getSrcEndpoint(task2.lightNode, task2.req));
                                        task2.req.setDstAddressMode(deCONZ::ApsExtAddress);

                                        light->setColorLoopActive(false);
                                        addTaskSetColorLoop(task2, false, 15);

                                        changed = true;
                                        colorloopDeactivated = true;
                                    }
                                    //turn on colorloop if scene was saved with colorloop (FLS don't save colorloop at device)
                                    else if (ls->colorloopActive() && light->isColorLoopActive() != ls->colorloopActive())
                                    {
                                        task2.lightNode = light;
                                        task2.req.dstAddress() = task2.lightNode->address();
                                        task2.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
                                        task2.req.setDstEndpoint(task2.lightNode->haEndpoint().endpoint());
                                        task2.req.setSrcEndpoint(getSrcEndpoint(task2.lightNode, task2.req));
                                        task2.req.setDstAddressMode(deCONZ::ApsExtAddress);

                                        light->setColorLoopActive(true);
                                        light->setColorLoopSpeed(ls->colorloopTime());
                                        addTaskSetColorLoop(task2, true, ls->colorloopTime());
                                        changed = true;
                                    }
                                    if (ls->on() && !light->isOn())
                                    {
                                        light->setIsOn(true);
                                        changed = true;
                                    }
                                    if (!ls->on() && light->isOn())
                                    {
                                        light->setIsOn(false);
                                        changed = true;
                                    }
                                    if ((uint16_t)ls->bri() != light->level())
                                    {
                                        light->setLevel((uint16_t)ls->bri());
                                        changed = true;
                                    }
                                    if (changed)
                                    {
                                        updateEtag(light->etag);
                                    }
                                }
                            }

                            //recall scene again
                            if (colorloopDeactivated)
                            {
                                callScene(group, sceneId);
                            }
                            break;
                        }
                    }
                }
                // turning 'on' the group is also a assumtion but a very likely one
                if (!group->isOn())
                {
                    group->setIsOn(true);
                    updateEtag(group->etag);
                }

                updateEtag(gwConfigEtag);

                processTasks();
            }
        }
    }
}

/*! Handle packets related to the ZCL On/Off cluster.
    \param task the task which belongs to this response
    \param ind the APS level data indication containing the ZCL packet
    \param zclFrame the actual ZCL frame which holds the scene cluster reponse
 */
void DeRestPluginPrivate::handleOnOffClusterIndication(TaskItem &task, const deCONZ::ApsDataIndication &ind, deCONZ::ZclFrame &zclFrame)
{
    Q_UNUSED(task);

    if (!ind.srcAddress().hasExt())
    {
        return;
    }

    // update Nodes and Groups state if On/Off Command was send by a switch
    Sensor *sensorNode = getSensorNodeForAddressAndEndpoint(ind.srcAddress().ext(), ind.srcEndpoint());

    if (sensorNode)
    {
        if (sensorNode->deletedState() != Sensor::StateDeleted)
        {
            std::vector<Group>::iterator i = groups.begin();
            std::vector<Group>::iterator end = groups.end();

            for (; i != end; ++i)
            {
                if (i->state() != Group::StateDeleted && i->state() != Group::StateDeleteFromDB)
                {
                    if (i->m_deviceMemberships.end() != std::find(i->m_deviceMemberships.begin(),
                                                                    i->m_deviceMemberships.end(),
                                                                    sensorNode->id()))
                    {
                       //found
                       if (zclFrame.commandId() == 0x00 || zclFrame.commandId() == 0x40) // Off || Off with effect
                       {
                           i->setIsOn(false);
                       }
                       else if (zclFrame.commandId() == 0x01) // On
                       {
                           i->setIsOn(true);
                           if (i->isColorLoopActive())
                           {
                               TaskItem task1;
                               task1.req.dstAddress().setGroup(i->address());
                               task1.req.setDstAddressMode(deCONZ::ApsGroupAddress);
                               task1.req.setDstEndpoint(0xFF); // broadcast endpoint
                               task1.req.setSrcEndpoint(getSrcEndpoint(0, task1.req));

                               addTaskSetColorLoop(task1, false, 15);
                               i->setColorLoopActive(false);
                           }
                       }
                       updateEtag(i->etag);

                       // check each light if colorloop needs to be disabled
                       std::vector<LightNode>::iterator l = nodes.begin();
                       std::vector<LightNode>::iterator lend = nodes.end();

                       for (; l != lend; ++l)
                       {
                           if (isLightNodeInGroup(&(*l),i->address()))
                           {
                               if (zclFrame.commandId() == 0x00 || zclFrame.commandId() == 0x40) // Off || Off with effect
                               {
                                   l->setIsOn(false);
                               }
                               else if (zclFrame.commandId() == 0x01) // On
                               {
                                   l->setIsOn(true);

                                   if (l->isAvailable() && l->state() != LightNode::StateDeleted && l->isColorLoopActive())
                                   {
                                       TaskItem task2;
                                       task2.lightNode = &(*l);
                                       task2.req.dstAddress() = task2.lightNode->address();
                                       task2.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
                                       task2.req.setDstEndpoint(task2.lightNode->haEndpoint().endpoint());
                                       task2.req.setSrcEndpoint(getSrcEndpoint(task2.lightNode, task2.req));
                                       task2.req.setDstAddressMode(deCONZ::ApsExtAddress);

                                       addTaskSetColorLoop(task2, false, 15);
                                       l->setColorLoopActive(false);
                                   }
                               }
                               updateEtag(l->etag);
                           }
                       }
                    }
                }
            }
            updateEtag(gwConfigEtag);
        }
        else if (sensorNode->deletedState() == Sensor::StateDeleted && gwPermitJoinDuration > 0)
        {
            // reactivate deleted switch and recover group
            sensorNode->setDeletedState(Sensor::StateNormal);

            std::vector<Group>::iterator g = groups.begin();
            std::vector<Group>::iterator gend = groups.end();

            for (; g != gend; ++g)
            {
                std::vector<QString> &v = g->m_deviceMemberships;

                if ((std::find(v.begin(), v.end(), sensorNode->id()) != v.end()) && (g->state() == Group::StateDeleted))
                {
                    g->setState(Group::StateNormal);
                    updateEtag(g->etag);
                    break;
                }
            }
            updateEtag(sensorNode->etag);

            std::vector<Sensor>::iterator s = sensors.begin();
            std::vector<Sensor>::iterator send = sensors.end();

            for (; s != send; ++s)
            {
                if (s->uniqueId() == sensorNode->uniqueId() && s->id() != sensorNode->id())
                {
                    s->setDeletedState(Sensor::StateNormal);
                    updateEtag(s->etag);

                    std::vector<Group>::iterator g = groups.begin();
                    std::vector<Group>::iterator gend = groups.end();

                    for (; g != gend; ++g)
                    {
                        std::vector<QString> &v = g->m_deviceMemberships;

                        if ((std::find(v.begin(), v.end(), s->id()) != v.end()) && (g->state() == Group::StateDeleted))
                        {
                            g->setState(Group::StateNormal);
                            updateEtag(g->etag);
                            break;
                        }
                    }
                }
            }

            updateEtag(gwConfigEtag);
            queSaveDb(DB_GROUPS | DB_SENSORS, DB_SHORT_SAVE_DELAY);
        }
    }
}

/*! Handle packets related to the ZCL Commissioning cluster.
    \param task the task which belongs to this response
    \param ind the APS level data indication containing the ZCL packet
    \param zclFrame the actual ZCL frame which holds the Commissioning cluster reponse
 */
void DeRestPluginPrivate::handleCommissioningClusterIndication(TaskItem &task, const deCONZ::ApsDataIndication &ind, deCONZ::ZclFrame &zclFrame)
{
    Q_UNUSED(task);

    if (!ind.srcAddress().hasExt())
    {
        return;
    }

    uint8_t ep = ind.srcEndpoint();
    Sensor *sensorNode = getSensorNodeForAddressAndEndpoint(ind.srcAddress().ext(),ep);
    //int endpointCount = getNumberOfEndpoints(ind.srcAddress().ext());
    int epIter = 0;

    if (!sensorNode)
    {
        return;
    }

    if (zclFrame.isDefaultResponse())
    {
    }
    else if (zclFrame.commandId() == 0x41) // Get group identifiers response
    {
        DBG_Assert(zclFrame.payload().size() >= 4);

        QDataStream stream(zclFrame.payload());
        stream.setByteOrder(QDataStream::LittleEndian);

        uint8_t total;
        uint8_t startIndex;
        uint8_t count;
        uint16_t groupId;
        uint8_t type;

        stream >> total;
        stream >> startIndex;
        stream >> count;

        DBG_Printf(DBG_INFO, "Get group identifiers response of sensor %s. Count: %u\n", qPrintable(sensorNode->address().toStringExt()), count);

        while (!stream.atEnd())
        {
            stream >> groupId;
            stream >> type;
            DBG_Printf(DBG_INFO, " - Id: %u, type: %u\n", groupId, type);

            Group *group1 = getGroupForId(groupId);

            if (epIter < count && ep != ind.srcEndpoint())
            {
                sensorNode = getSensorNodeForAddressAndEndpoint(ind.srcAddress().ext(), ep);
                if (!sensorNode)
                {
                    sensorNode = getSensorNodeForAddressAndEndpoint(ind.srcAddress().ext(), ind.srcEndpoint());
                }
            }
            epIter++;
            // assumption: different groups from consecutive endpoints
            ep++;

            if (sensorNode->deletedState() != Sensor::StateDeleted)
            {
                if (group1)
                {
                    if (group1->state() == Group::StateDeleted)
                    {
                        group1->setState(Group::StateNormal);
                    }
                    if (group1->m_deviceMemberships.end() == std::find(group1->m_deviceMemberships.begin(),
                                                                        group1->m_deviceMemberships.end(),
                                                                        sensorNode->id()))
                    {
                        //not found
                        group1->m_deviceMemberships.push_back(sensorNode->id());
                    }

                    // put coordinator into group
                    // deCONZ firmware will put itself into a group after sending out a groupcast
                    // therefore we will receives commands to the same group
                    TaskItem task;
                    task.req.setDstAddressMode(deCONZ::ApsGroupAddress);
                    task.req.dstAddress().setGroup(group1->address());
                    task.req.setDstEndpoint(0xFF); // broadcast endpoint
                    task.req.setSrcEndpoint(getSrcEndpoint(0, task.req));
                    if (!addTaskViewGroup(task, group1->address()))
                    {
                        DBG_Printf(DBG_INFO, "failed to send view group\n");
                    }

                    queSaveDb(DB_GROUPS, DB_SHORT_SAVE_DELAY);
                    updateEtag(group1->etag);
                }
                else
                {
                    // delete older groups of this switch permanently
                    std::vector<Group>::iterator i = groups.begin();
                    std::vector<Group>::iterator end = groups.end();

                    for (; i != end; ++i)
                    {
                        if (i->m_deviceMemberships.end() != std::find(i->m_deviceMemberships.begin(),
                                                                    i->m_deviceMemberships.end(),
                                                                    sensorNode->id()))
                        {
                            //found
                            if (i->state() == Group::StateDeleted)
                            {
                                i->setState(Group::StateDeleteFromDB);
                            }
                        }
                    }

                    //create new switch group
                    Group group;
                    group.setAddress(groupId);
                    group.m_deviceMemberships.push_back(sensorNode->id());
                    group.colorX = 0;
                    group.colorY = 0;
                    group.setIsOn(false);
                    group.level = 128;
                    group.hue = 0;
                    group.hueReal = 0.0f;
                    group.sat = 128;
                    group.setName(QString());
                    if (group.name().isEmpty())
                    {
                        group.setName(QString("%1").arg(sensorNode->name()));
                    }

                    updateEtag(group.etag);
                    groups.push_back(group);
                    sensorNode->setMode(2); // sensor was reset -> set mode to '2 groups'
                    queSaveDb(DB_GROUPS | DB_SENSORS, DB_SHORT_SAVE_DELAY);

                    // put coordinator into group
                    // deCONZ firmware will put itself into a group after sending out a groupcast
                    // therefore we will receives commands to the same group
                    TaskItem task2;
                    task2.req.setDstAddressMode(deCONZ::ApsGroupAddress);
                    task2.req.dstAddress().setGroup(group.address());
                    task2.req.setDstEndpoint(0xFF); // broadcast endpoint
                    task2.req.setSrcEndpoint(getSrcEndpoint(0, task2.req));
                    if (!addTaskViewGroup(task2, group.address()))
                    {
                        DBG_Printf(DBG_INFO, "failed to send view group\n");
                    }
                }
                updateEtag(gwConfigEtag);
            }
        }
    }
}

/*! Handle the case that a node (re)joins the network.
    \param ind a ZDP DeviceAnnce_req
 */
void DeRestPluginPrivate::handleDeviceAnnceIndication(const deCONZ::ApsDataIndication &ind)
{
    if (!ind.srcAddress().hasExt())
    {
        return;
    }

    std::vector<LightNode>::iterator i = nodes.begin();
    std::vector<LightNode>::iterator end = nodes.end();

    for (; i != end; ++i)
    {
        deCONZ::Node *node = i->node();
        if (node && i->address().ext() == ind.srcAddress().ext())
        {
            if (node->endpoints().end() == std::find(node->endpoints().begin(),
                                                     node->endpoints().end(),
                                                     i->haEndpoint().endpoint()))
            {
                continue; // not a active endpoint
            }

            if (!i->isAvailable())
            {
                i->setIsAvailable(true);
                updateEtag(gwConfigEtag);
            }

            DBG_Printf(DBG_INFO, "DeviceAnnce of LightNode: %s\n", qPrintable(ind.srcAddress().toStringExt()));

            // force reading attributes
            i->setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
            i->setLastRead(idleTotalCounter);

            i->enableRead(READ_MODEL_ID |
                          READ_SWBUILD_ID |
                          READ_COLOR |
                          READ_LEVEL |
                          READ_ON_OFF |
                          READ_GROUPS |
                          READ_SCENES);
            i->setSwBuildId(QString()); // might be changed due otau
            updateEtag(i->etag);
        }
    }

    std::vector<Sensor>::iterator si = sensors.begin();
    std::vector<Sensor>::iterator send = sensors.end();

    for (; si != send; ++si)
    {
        if (si->address().ext() == ind.srcAddress().ext())
        {
            DBG_Printf(DBG_INFO, "DeviceAnnce of SensorNode: %s\n", qPrintable(ind.srcAddress().toStringExt()));
            checkSensorNodeReachable(&(*si));
            /*
            if (si->deletedState() == Sensor::StateDeleted)
            {
                si->setIsAvailable(true);
                si->setNextReadTime(QTime::currentTime().addMSecs(ReadAttributesLongDelay));
                si->enableRead(READ_BINDING_TABLE | READ_GROUP_IDENTIFIERS | READ_MODEL_ID | READ_SWBUILD_ID);
                si->setLastRead(idleTotalCounter);
                si->setDeletedState(Sensor::StateNormal);

                updateEtag(si->etag);
                updateEtag(gwConfigEtag);
                queSaveDb(DB_SENSORS, DB_SHORT_SAVE_DELAY);
            }
            */
        }
    }
}

/*! Mark node so current state will be pushed to all clients.
 */
void DeRestPluginPrivate::markForPushUpdate(LightNode *lightNode)
{
    std::list<LightNode*>::iterator i = std::find(broadCastUpdateNodes.begin(), broadCastUpdateNodes.end(), lightNode);

    if (i == broadCastUpdateNodes.end())
    {
        broadCastUpdateNodes.push_back(lightNode);
    }
}

/*! Push data from a task into all LightNodes of a group or single LightNode.
 */
void DeRestPluginPrivate::taskToLocalData(const TaskItem &task)
{
    Group *group;
    Group dummyGroup;
    std::vector<LightNode*> pushNodes;

    if (task.req.dstAddress().hasGroup() || task.req.dstAddress().isNwkBroadcast())
    {
        group = getGroupForId(task.req.dstAddress().group());

        DBG_Assert(group != 0);

        if (!group)
        {
            group = &dummyGroup;
        }

        std::vector<LightNode>::iterator i = nodes.begin();
        std::vector<LightNode>::iterator end = nodes.end();

        for (; i != end; ++i)
        {
            LightNode *lightNode = &(*i);
            if (isLightNodeInGroup(lightNode, task.req.dstAddress().group()))
            {
                pushNodes.push_back(lightNode);
            }
        }
    }
    else if (task.req.dstAddress().hasExt())
    {
        group = &dummyGroup; // never mind
        LightNode *lightNode = getLightNodeForAddress(task.req.dstAddress().ext(), task.req.dstEndpoint());
        if (lightNode)
        {
            pushNodes.push_back(lightNode);
        }
    }
    else
    {
        return;
    }

    std::vector<LightNode*>::iterator i = pushNodes.begin();
    std::vector<LightNode*>::iterator end = pushNodes.end();

    switch (task.taskType)
    {
    case TaskSendOnOffToggle:
        updateEtag(group->etag);
        group->setIsOn(task.onOff);
        break;

    case TaskSetLevel:
        if (task.level > 0)
        {
            group->setIsOn(true);
        }
        else
        {
            group->setIsOn(false);
        }
        updateEtag(group->etag);
        group->level = task.level;
        break;

    case TaskSetSat:
        updateEtag(group->etag);
        group->sat = task.sat;
        break;

    case TaskSetEnhancedHue:
        updateEtag(group->etag);
        group->hue = task.hue;
        group->hueReal = task.hueReal;
        break;

    case TaskSetHueAndSaturation:
        updateEtag(group->etag);
        group->sat = task.sat;
        group->hue = task.hue;
        group->hueReal = task.hueReal;
        break;

    case TaskSetXyColor:
        updateEtag(group->etag);
        group->colorX = task.colorX;
        group->colorY = task.colorY;
        break;

    case TaskSetColorTemperature:
        updateEtag(group->etag);
        group->colorTemperature = task.colorTemperature;
        break;

    case TaskSetColorLoop:
        updateEtag(group->etag);
        group->setColorLoopActive(task.colorLoop);
        break;

    default:
        break;
    }

    for (; i != end; ++i)
    {
        LightNode *lightNode = *i;

        switch (task.taskType)
        {
        case TaskSendOnOffToggle:
            updateEtag(lightNode->etag);
            lightNode->setIsOn(task.onOff);
            setAttributeOnOff(lightNode);
            break;

        case TaskSetLevel:
            if (task.level > 0)
            {
                lightNode->setIsOn(true);
            }
            else
            {
                lightNode->setIsOn(false);
            }
            updateEtag(lightNode->etag);
            lightNode->setLevel(task.level);
            setAttributeLevel(lightNode);
            setAttributeOnOff(lightNode);
            break;

        case TaskStopLevel:
            updateEtag(lightNode->etag);
            lightNode->enableRead(READ_LEVEL);
            lightNode->mustRead(READ_LEVEL);
            break;

        case TaskSetSat:
            updateEtag(lightNode->etag);
            lightNode->setSaturation(task.sat);
            setAttributeSaturation(lightNode);
            break;

        case TaskSetEnhancedHue:
            updateEtag(lightNode->etag);
            lightNode->setEnhancedHue(task.enhancedHue);
            setAttributeEnhancedHue(lightNode);
            break;

        case TaskSetHueAndSaturation:
            updateEtag(lightNode->etag);
            lightNode->setSaturation(task.sat);
            lightNode->setEnhancedHue(task.enhancedHue);
            setAttributeSaturation(lightNode);
            setAttributeEnhancedHue(lightNode);
            break;

        case TaskSetXyColor:
            updateEtag(lightNode->etag);
            lightNode->setColorXY(task.colorX, task.colorY);
            setAttributeColorXy(lightNode);
            break;

        case TaskSetColorTemperature:
            updateEtag(lightNode->etag);
            lightNode->setColorTemperature(task.colorTemperature);
            setAttributeColorTemperature(lightNode);
            break;

        case TaskSetColorLoop:
            if (lightNode->colorMode() == "ct" || (lightNode->colorX() == 0 && lightNode->colorY() == 0 && lightNode->hue() == 0 && lightNode->enhancedHue() == 0))
            {
                //do nothing
            }
            else
            {
                updateEtag(lightNode->etag);
                lightNode->setColorLoopActive(task.colorLoop);
                setAttributeColorLoopActive(lightNode);
            }
            break;

        default:
            break;
        }
    }
}

/*! Updates the onOff attribute in the local node cache.
 */
void DeRestPluginPrivate::setAttributeOnOff(LightNode *lightNode)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode || !lightNode->node())
    {
        return;
    }

    deCONZ::ZclCluster *cl = getInCluster(lightNode->node(), lightNode->haEndpoint().endpoint(), ONOFF_CLUSTER_ID);

    if (cl && cl->attributes().size() > 0)
    {
        deCONZ::ZclAttribute &attr = cl->attributes()[0];

        DBG_Assert(attr.id() == 0x0000);

        if (attr.id() == 0x0000)
        {
            attr.setValue(lightNode->isOn());
        }
    }
}

/*! Updates the level attribute in the local node cache.
 */
void DeRestPluginPrivate::setAttributeLevel(LightNode *lightNode)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode || !lightNode->node())
    {
        return;
    }

    deCONZ::ZclCluster *cl = getInCluster(lightNode->node(), lightNode->haEndpoint().endpoint(), LEVEL_CLUSTER_ID);

    if (cl && cl->attributes().size() > 0)
    {
        deCONZ::ZclAttribute &attr = cl->attributes()[0];
        if (attr.id() == 0x0000)
        {
            attr.setValue((quint64)lightNode->level());
        }
    }
}

/*! Updates the saturation attribute in the local node cache.
 */
void DeRestPluginPrivate::setAttributeSaturation(LightNode *lightNode)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode || !lightNode->node())
    {
        return;
    }

    deCONZ::ZclCluster *cl = getInCluster(lightNode->node(), lightNode->haEndpoint().endpoint(), COLOR_CLUSTER_ID);

    if (cl)
    {
        std::vector<deCONZ::ZclAttribute>::iterator i = cl->attributes().begin();
        std::vector<deCONZ::ZclAttribute>::iterator end = cl->attributes().end();

        for (; i != end; ++i)
        {
            if (i->id() == 0x0001) // Current saturation
            {
                i->setValue((quint64)lightNode->saturation());
                break;
            }

        }
    }
}

/*! Updates the color xy attribute in the local node cache.
 */
void DeRestPluginPrivate::setAttributeColorXy(LightNode *lightNode)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode || !lightNode->node())
    {
        return;
    }

    deCONZ::ZclCluster *cl = getInCluster(lightNode->node(), lightNode->haEndpoint().endpoint(), COLOR_CLUSTER_ID);

    if (cl)
    {
        std::vector<deCONZ::ZclAttribute>::iterator i = cl->attributes().begin();
        std::vector<deCONZ::ZclAttribute>::iterator end = cl->attributes().end();

        for (; i != end; ++i)
        {
            if (i->id() == 0x0003) // Current color x
            {
                i->setValue((quint64)lightNode->colorX());
            }
            else if (i->id() == 0x0004) // Current color y
            {
                i->setValue((quint64)lightNode->colorY());
                break;
            }
        }
    }
}

/*! Updates the color temperature attribute in the local node cache.
 */
void DeRestPluginPrivate::setAttributeColorTemperature(LightNode *lightNode)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode || !lightNode->node())
    {
        return;
    }

    deCONZ::ZclCluster *cl = getInCluster(lightNode->node(), lightNode->haEndpoint().endpoint(), COLOR_CLUSTER_ID);

    if (cl)
    {
        std::vector<deCONZ::ZclAttribute>::iterator i = cl->attributes().begin();
        std::vector<deCONZ::ZclAttribute>::iterator end = cl->attributes().end();

        for (; i != end; ++i)
        {
            if (i->id() == 0x0007) // Current color temperature
            {
                i->setValue((quint64)lightNode->colorTemperature());
                break;
            }
        }
    }
}

/*! Updates the color loop active attribute in the local node cache.
 */
void DeRestPluginPrivate::setAttributeColorLoopActive(LightNode *lightNode)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode || !lightNode->node())
    {
        return;
    }

    deCONZ::ZclCluster *cl = getInCluster(lightNode->node(), lightNode->haEndpoint().endpoint(), COLOR_CLUSTER_ID);

    if (cl)
    {
        std::vector<deCONZ::ZclAttribute>::iterator i = cl->attributes().begin();
        std::vector<deCONZ::ZclAttribute>::iterator end = cl->attributes().end();

        for (; i != end; ++i)
        {
            if (i->id() == 0x4002) // Color loop active
            {
                i->setValue(lightNode->isColorLoopActive());
                break;
            }
        }
    }
}

/*! Shall be called whenever the user did something which resulted in a over the air request.
 */
void DeRestPluginPrivate::userActivity()
{
    idleLastActivity = 0;
}

/*! Updates the enhanced hue attribute in the local node cache.
 */
void DeRestPluginPrivate::setAttributeEnhancedHue(LightNode *lightNode)
{
    DBG_Assert(lightNode != 0);

    if (!lightNode || !lightNode->node())
    {
        return;
    }

    deCONZ::ZclCluster *cl = getInCluster(lightNode->node(), lightNode->haEndpoint().endpoint(), COLOR_CLUSTER_ID);

    if (cl)
    {
        std::vector<deCONZ::ZclAttribute>::iterator i = cl->attributes().begin();
        std::vector<deCONZ::ZclAttribute>::iterator end = cl->attributes().end();

        for (; i != end; ++i)
        {
            if (i->id() == 0x4000) // Enhanced hue
            {
                i->setValue((quint64)lightNode->enhancedHue());
                break;
            }

        }
    }
}

/*! Main plugin constructor.
    \param parent - parent object
 */
DeRestPlugin::DeRestPlugin(QObject *parent) :
    QObject(parent)
{
    d = new DeRestPluginPrivate(this);
    d->q_ptr = this;
    m_state = StateOff;
    m_w = 0;
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(false);

    connect(QCoreApplication::instance(), SIGNAL(aboutToQuit()),
            this, SLOT(appAboutToQuit()));

    connect(m_idleTimer, SIGNAL(timeout()),
            this, SLOT(idleTimerFired()));

    m_readAttributesTimer = new QTimer(this);
    m_readAttributesTimer->setSingleShot(true);

    connect(m_readAttributesTimer, SIGNAL(timeout()),
            this, SLOT(checkZclAttributeTimerFired()));

    m_idleTimer->start(1000);
}

/*! The plugin deconstructor.
 */
DeRestPlugin::~DeRestPlugin()
{
    d = 0;
}

/*! Handle idle states.

    After IDLE_LIMIT seconds user inactivity this timer
    checks if nodes need to be refreshed. This is the case
    if a node was not refreshed for IDLE_READ_LIMIT seconds.
 */
void DeRestPlugin::idleTimerFired()
{
    d->idleTotalCounter++;
    d->idleLastActivity++;

    if (d->idleTotalCounter < 0) // overflow
    {
        d->idleTotalCounter = 0;
    }

    if (d->idleLastActivity < 0) // overflow
    {
        d->idleLastActivity = 0;
    }

    if (d->idleLimit > 0)
    {
        d->idleLimit--;
    }

    if (d->idleLastActivity < IDLE_USER_LIMIT)
    {
        return;
    }

    if (!pluginActive())
    {
        return;
    }

    // put coordinator into groups of switches
    // deCONZ firmware will put itself into a group after sending out a groupcast
    // therefore we will receives commands to the same group
    if (!d->groupDeviceMembershipChecked)
    {
        TaskItem task;

        std::vector<Group>::const_iterator i = d->groups.begin();
        std::vector<Group>::const_iterator end = d->groups.end();

        for (; i != end; ++i)
        {
            if (/*i->state() == Group::StateNormal && */i->m_deviceMemberships.size() > 0)
            {
                task.req.setDstAddressMode(deCONZ::ApsGroupAddress);
                task.req.dstAddress().setGroup(i->address());
                task.req.setDstEndpoint(0xFF); // broadcast endpoint
                task.req.setSrcEndpoint(d->getSrcEndpoint(0, task.req));
                task.req.setRadius(1);
                if (!d->addTaskViewGroup(task, i->address()))
                {
                    DBG_Printf(DBG_INFO, "failed to send view group\n");
                }
                else
                {
                    d->groupDeviceMembershipChecked = true;
                }
            }
        }
    }

    bool processLights = false;

    if (d->idleLimit <= 0)
    {
        DBG_Printf(DBG_INFO_L2, "Idle timer triggered\n");

        if (!d->nodes.empty())
        {
            if (d->lightIter >= d->nodes.size())
            {
                d->lightIter = 0;
            }

            while (d->lightIter < d->nodes.size())
            {
                LightNode *lightNode = &d->nodes[d->lightIter];
                d->lightIter++;

                if (!lightNode->isAvailable())
                {
                    continue;
                }

                if (processLights)
                {
                    break;
                }

                if (lightNode->lastRead() < (d->idleTotalCounter - IDLE_READ_LIMIT))
                {
                    lightNode->enableRead(READ_ON_OFF | READ_LEVEL | READ_COLOR | READ_GROUPS | READ_SCENES /*| READ_BINDING_TABLE*/);

                    if (lightNode->modelId().isEmpty() && !lightNode->mustRead(READ_MODEL_ID))
                    {
                        lightNode->enableRead(READ_MODEL_ID);
                        processLights = true;
                    }
                    if (lightNode->swBuildId().isEmpty() && !lightNode->mustRead(READ_SWBUILD_ID))
                    {
                        lightNode->enableRead(READ_SWBUILD_ID);
                        processLights = true;
                    }
                    if ((lightNode->manufacturer().isEmpty() || lightNode->manufacturer() == "Unknown") && !lightNode->mustRead(READ_SWBUILD_ID))
                    {
                        lightNode->enableRead(READ_VENDOR_NAME);
                        processLights = true;
                    }
                    lightNode->setNextReadTime(QTime::currentTime());
                    lightNode->setLastRead(d->idleTotalCounter);
                    DBG_Printf(DBG_INFO, "Force read attributes for node %s\n", qPrintable(lightNode->name()));
                }

                if (lightNode->lastAttributeReportBind() < (d->idleTotalCounter - IDLE_ATTR_REPORT_BIND_LIMIT))
                {
                    d->checkLightBindingsForAttributeReporting(lightNode);
                    lightNode->setLastAttributeReportBind(d->idleTotalCounter);
                    DBG_Printf(DBG_INFO, "Force binding of attribute reporting for node %s\n", qPrintable(lightNode->name()));
                    processLights = true;
                }
            }
        }

        bool processSensors = false;

        if (!d->sensors.empty())
        {
            if (d->sensorIter >= d->sensors.size())
            {
                d->sensorIter = 0;
            }

            while (d->sensorIter < d->sensors.size())
            {
                Sensor *sensorNode = &d->sensors[d->sensorIter];
                d->sensorIter++;

                if (!sensorNode->isAvailable())
                {
                    continue;
                }

                if (processSensors)
                {
                    break;
                }

                if (sensorNode->modelId().isEmpty())
                {
                    LightNode *lightNode = d->getLightNodeForAddress(sensorNode->address().ext());
                    if (lightNode && !lightNode->modelId().isEmpty())
                    {
                        sensorNode->setModelId(lightNode->modelId());
                    }
                    else
                    {
                        sensorNode->enableRead(READ_MODEL_ID);
                        processSensors = true;
                    }
                }

                if (sensorNode->manufacturer().isEmpty() ||
                    sensorNode->manufacturer() == QLatin1String("unknown"))
                {
                    sensorNode->enableRead(READ_VENDOR_NAME);
                    processSensors = true;
                }

                if (sensorNode->lastRead() < (d->idleTotalCounter - IDLE_READ_LIMIT))
                {
                    bool checkBindingTable = false;
                    sensorNode->setLastRead(d->idleTotalCounter);
                    sensorNode->setNextReadTime(QTime::currentTime());

                    {
                        std::vector<quint16>::const_iterator ci = sensorNode->fingerPrint().inClusters.begin();
                        std::vector<quint16>::const_iterator cend = sensorNode->fingerPrint().inClusters.end();
                        for (;ci != cend; ++ci)
                        {
                            NodeValue val;

                            if (*ci == ILLUMINANCE_MEASUREMENT_CLUSTER_ID)
                            {
                                val = sensorNode->getZclValue(*ci, 0x0000); // measured value
                            }
                            else if (*ci == OCCUPANCY_SENSING_CLUSTER_ID)
                            {
                                val = sensorNode->getZclValue(*ci, 0x0000); // occupied state
                            }

                            if (val.timestampLastReport.isValid() &&
                                val.timestampLastReport.secsTo(QTime::currentTime()) < (60 * 45)) // got update in timely manner
                            {
                                DBG_Printf(DBG_INFO, "binding for attribute reporting SensorNode %s of cluster 0x%04X seems to be active\n", qPrintable(sensorNode->name()), *ci);
                            }
                            else
                            {
                                checkBindingTable = true;
                            }

                            if (*ci == OCCUPANCY_SENSING_CLUSTER_ID)
                            {
                                if (!sensorNode->mustRead(READ_OCCUPANCY_CONFIG))
                                {
                                    sensorNode->enableRead(READ_OCCUPANCY_CONFIG);
                                    processSensors = true;
                                }
                            }
                        }
                    }


                    if (checkBindingTable && !sensorNode->mustRead(READ_BINDING_TABLE))
                    {
                        sensorNode->enableRead(READ_BINDING_TABLE);
                        processSensors = true;
                    }

                    DBG_Printf(DBG_INFO, "Force read attributes for SensorNode %s\n", qPrintable(sensorNode->name()));
                    //break;
                }

                if (sensorNode->lastAttributeReportBind() < (d->idleTotalCounter - IDLE_ATTR_REPORT_BIND_LIMIT))
                {
                    d->checkSensorBindingsForAttributeReporting(sensorNode);
                    sensorNode->setLastAttributeReportBind(d->idleTotalCounter);
                    DBG_Printf(DBG_INFO, "Force binding of attribute reporting for node %s\n", qPrintable(sensorNode->name()));
                    processSensors = true;
                }
            }
        }

        {
            std::vector<LightNode>::iterator i = d->nodes.begin();
            std::vector<LightNode>::iterator end = d->nodes.end();

            int countNoColorXySupport = 0;

            for (; i != end; ++i)
            {
                // older FLS which do not have correct support for color mode xy has atmel vendor id
                if (i->isAvailable() && (i->manufacturerCode() == VENDOR_ATMEL))
                {
                    countNoColorXySupport++;
                }
            }

            if ((countNoColorXySupport > 0) && d->supportColorModeXyForGroups)
            {
                DBG_Printf(DBG_INFO_L2, "disable support for CIE 1931 XY color mode for groups\n");
                d->supportColorModeXyForGroups = false;
            }
            else if ((countNoColorXySupport == 0) && !d->supportColorModeXyForGroups)
            {
                DBG_Printf(DBG_INFO_L2, "enable support for CIE 1931 XY color mode for groups\n");
                d->supportColorModeXyForGroups = true;
            }
            else
            {
    //            DBG_Printf(DBG_INFO_L2, "support for CIE 1931 XY color mode for groups %u\n", d->supportColorModeXyForGroups);
            }
        }

        startZclAttributeTimer(checkZclAttributesDelay);

        if (processLights || processSensors)
        {
            d->idleLimit = 1;
        }
        else
        {
            d->idleLimit = IDLE_LIMIT;
        }
    }
}

/*! Refresh all nodes by forcing the idle timer to trigger.
 */
void DeRestPlugin::refreshAll()
{
    std::vector<LightNode>::iterator i = d->nodes.begin();
    std::vector<LightNode>::iterator end = d->nodes.end();

    for (; i != end; ++i)
    {
        // force refresh on next idle timer timeout
        i->setLastRead(d->idleTotalCounter - (IDLE_READ_LIMIT + 1));
    }

    d->idleLimit = 0;
    d->idleLastActivity = IDLE_USER_LIMIT;
    d->runningTasks.clear();
    d->tasks.clear();
}

/*! Starts the read attributes timer with a given \p delay.
 */
void DeRestPlugin::startZclAttributeTimer(int delay)
{
    if (!m_readAttributesTimer->isActive())
    {
        m_readAttributesTimer->start(delay);
    }
}

/*! Stops the read attributes timer.
 */
void DeRestPlugin::stopZclAttributeTimer()
{
    m_readAttributesTimer->stop();
}

/*! Checks if attributes of any nodes shall be queried or written.
 */
void DeRestPlugin::checkZclAttributeTimerFired()
{
    if (!pluginActive())
    {
        return;
    }

    stopZclAttributeTimer();

    if (d->lightAttrIter >= d->nodes.size())
    {
        d->lightAttrIter = 0;
    }

    while (d->lightAttrIter < d->nodes.size())
    {
        LightNode *lightNode = &d->nodes[d->lightAttrIter];
        d->lightAttrIter++;

        if (d->processZclAttributes(lightNode))
        {
            // read next later
            startZclAttributeTimer(checkZclAttributesDelay);
            d->processTasks();
            break;
        }
    }

    if (d->sensorAttrIter >= d->sensors.size())
    {
        d->sensorAttrIter = 0;
    }

    while (d->sensorAttrIter < d->sensors.size())
    {
        Sensor *sensorNode = &d->sensors[d->sensorAttrIter];
        d->sensorAttrIter++;

        if (d->processZclAttributes(sensorNode))
        {
            // read next later
            startZclAttributeTimer(checkZclAttributesDelay);
            d->processTasks();
            break;
        }
    }

    startZclAttributeTimer(checkZclAttributesDelay);
}

/*! Handler called before the application will be closed.
 */
void DeRestPlugin::appAboutToQuit()
{
    DBG_Printf(DBG_INFO, "REST API plugin shutting down\n");

    if (d)
    {
        d->openDb();
        d->saveDb();
        d->closeDb();

        d->apsCtrl = 0;
    }
}

/*! Query this plugin which features are supported.
    \param feature - feature to be checked
    \return true if supported
 */
bool DeRestPlugin::hasFeature(Features feature)
{
    switch (feature)
    {
    case DialogFeature:
    case HttpClientHandlerFeature:
        return true;

    default:
        break;
    }

    return false;
}

/*! Creates a control widget for this plugin.
    \return 0 - not implemented
 */
QWidget *DeRestPlugin::createWidget()
{
    return 0;
}

/*! Creates a control dialog for this plugin.
    \return the dialog
 */
QDialog *DeRestPlugin::createDialog()
{
    if (!m_w)
    {
        m_w = new DeRestWidget(0);

        connect(m_w, SIGNAL(refreshAllClicked()),
                this, SLOT(refreshAll()));

        connect(m_w, SIGNAL(changeChannelClicked(quint8)),
                d, SLOT(changeChannel(quint8)));
    }

    return m_w;
}

/*! Checks if a request is addressed to this plugin.
    \param hdr - the http header of the request
    \return true - if the request could be processed
 */
bool DeRestPlugin::isHttpTarget(const QHttpRequestHeader &hdr)
{
    if (hdr.path().startsWith("/api/config"))
    {
        return true;
    }
    else if (hdr.path().startsWith("/api"))
    {
        QString path = hdr.path();
        int quest = path.indexOf('?');

        if (quest > 0)
        {
            path = path.mid(0, quest);
        }

        QStringList ls = path.split("/", QString::SkipEmptyParts);

        if (ls.size() > 2)
        {
            if ((ls[2] == "lights") ||
                (ls[2] == "groups") ||
                (ls[2] == "config") ||
                (ls[2] == "schedules") ||
                (ls[2] == "sensors") ||
                (ls[2] == "touchlink") ||
                (ls[2] == "rules") ||
                (hdr.path().at(4) != '/') /* Bug in some clients */)
            {
                return true;
            }
        }
        else // /api, /api/config and /api/287398279837
        {
            return true;
        }
    }
    else if (hdr.path().startsWith("/description.xml"))
    {
        if (!d->descriptionXml.isEmpty())
        {
            return true;
        }
    }

    return false;
}

/*! Broker for any incoming REST API request.
    \param hdr - http request header
    \param sock - the client socket
    \return 0 - on success
           -1 - on error
 */
int DeRestPlugin::handleHttpRequest(const QHttpRequestHeader &hdr, QTcpSocket *sock)
{
    QString content;
    QTextStream stream(sock);
    QHttpRequestHeader hdrmod(hdr);

    stream.setCodec(QTextCodec::codecForName("UTF-8"));

    if (m_state == StateOff)
    {
        if (d->apsCtrl && (d->apsCtrl->networkState() == deCONZ::InNetwork))
        {
            m_state = StateIdle;
        }
    }

    QUrl url(hdrmod.path()); // get rid of query string
    QString strpath = url.path();

    if (hdrmod.path().startsWith("/api"))
    {
        // some clients send /api123 instead of /api/123
        // correct the path here
        if (hdrmod.path().length() > 4 && hdrmod.path().at(4) != '/')
        {
            strpath.insert(4, '/');
        }
    }

    hdrmod.setRequest(hdrmod.method(), strpath);

    DBG_Printf(DBG_HTTP, "HTTP API %s %s - %s\n", qPrintable(hdr.method()), qPrintable(hdrmod.path()), qPrintable(sock->peerAddress().toString()));

    //qDebug() << hdr.toString();

    if (!stream.atEnd())
    {
        content = stream.readAll();
        DBG_Printf(DBG_HTTP, "\t%s\n", qPrintable(content));
    }

    connect(sock, SIGNAL(destroyed()),
            d, SLOT(clientSocketDestroyed()));

    QStringList path = hdrmod.path().split("/", QString::SkipEmptyParts);
    ApiRequest req(hdrmod, path, sock, content);
    ApiResponse rsp;

    rsp.httpStatus = HttpStatusNotFound;
    rsp.contentType = HttpContentHtml;

    int ret = REQ_NOT_HANDLED;

    // general response to a OPTIONS HTTP method
    if (req.hdr.method() == "OPTIONS")
    {
        stream << "HTTP/1.1 200 OK\r\n";
        stream << "Cache-Control: no-store, no-cache, must-revalidate, post-check=0, pre-check=0\r\n";
        stream << "Pragma: no-cache\r\n";
        stream << "Connection: close\r\n";
        stream << "Access-Control-Max-Age: 0\r\n";
        stream << "Access-Control-Allow-Origin: *\r\n";
        stream << "Access-Control-Allow-Credentials: true\r\n";
        stream << "Access-Control-Allow-Methods: POST, GET, OPTIONS, PUT, DELETE\r\n";
        stream << "Access-Control-Allow-Headers: Content-Type\r\n";
        stream << "Content-type: text/html\r\n";
        stream << "Content-Length: 0\r\n";
        stream << "\r\n";
        req.sock->flush();
        return 0;
    }

    if (path.size() > 2)
    {
        if (path[2] == "lights")
        {
            ret = d->handleLightsApi(req, rsp);
        }
        else if (path[2] == "groups")
        {
            ret = d->handleGroupsApi(req, rsp);
        }
        else if (path[2] == "schedules")
        {
            ret = d->handleSchedulesApi(req, rsp);
        }
        else if (path[2] == "touchlink")
        {
            ret = d->handleTouchlinkApi(req, rsp);
        }
        else if (path[2] == "sensors")
        {
            ret = d->handleSensorsApi(req, rsp);
        }
        else if (path[2] == "rules")
        {
            ret = d->handleRulesApi(req, rsp);
        }
    }

    if (ret == REQ_NOT_HANDLED)
    {
        ret = d->handleConfigurationApi(req, rsp);
    }

    if (ret == REQ_DONE)
    {
        return 0;
    }
    else if (ret == REQ_READY_SEND)
    {
        // new api // TODO cleanup/remove later
        // sending below
    }
    else if (hdr.path().startsWith("/description.xml") && (hdr.method() == "GET"))
    {
        rsp.httpStatus = HttpStatusOk;
        rsp.contentType = HttpContentHtml;
        rsp.str = d->descriptionXml;

        if (d->descriptionXml.isEmpty())
        {
            return -1;
        }
        stream << "HTTP/1.1 " << HttpStatusOk << "\r\n";
        stream << "Content-Type: application/xml\r\n";
        stream << "Content-Length:" << QString::number(d->descriptionXml.size()) << "\r\n";
        stream << "Connection: close\r\n";
        d->pushClientForClose(sock, 2);
        stream << "\r\n";
        stream << d->descriptionXml.constData();
        stream.flush();
        return 0;

    }
    else
    {
        DBG_Printf(DBG_HTTP, "%s unknown request: %s\n", Q_FUNC_INFO, qPrintable(hdr.path()));
    }

    QString str;

    if (!rsp.map.isEmpty())
    {
        rsp.contentType = HttpContentJson;
        str.append(Json::serialize(rsp.map));
    }
    else if (!rsp.list.isEmpty())
    {
        rsp.contentType = HttpContentJson;
        str.append(Json::serialize(rsp.list));
    }
    else if (!rsp.str.isEmpty())
    {
        rsp.contentType = HttpContentJson;
        str = rsp.str;
    }

    stream << "HTTP/1.1 " << rsp.httpStatus << "\r\n";
    stream << "Content-Type: " << rsp.contentType << "\r\n";
    stream << "Content-Length:" << QString::number(str.toUtf8().size()) << "\r\n";

    bool keepAlive = false;
    if (hdr.hasKey("Connection"))
    {
        if (hdr.value("Connection").toLower() == "keep-alive")
        {
            keepAlive = true;
            d->pushClientForClose(sock, 3);
        }
    }
    if (!keepAlive)
    {
        stream << "Connection: close\r\n";
        d->pushClientForClose(sock, 2);
    }

    if (!rsp.hdrFields.empty())
    {
        QList<QPair<QString, QString> >::iterator i = rsp.hdrFields.begin();
        QList<QPair<QString, QString> >::iterator end = rsp.hdrFields.end();

        for (; i != end; ++i)
        {
            stream << i->first << ": " <<  i->second << "\r\n";
        }
    }

    if (!rsp.etag.isEmpty())
    {
        stream << "ETag:" << rsp.etag  << "\r\n";
    }
    stream << "\r\n";

    if (!str.isEmpty())
    {
        stream << str;
    }

    stream.flush();
    if (!str.isEmpty())
    {
        DBG_Printf(DBG_HTTP, "%s\n", qPrintable(str));
    }

    return 0;
}

/*! A client socket was disconnected cleanup here.
    \param sock - the client
 */
void DeRestPlugin::clientGone(QTcpSocket *sock)
{
    d->eventListeners.remove(sock);
}

bool DeRestPlugin::pluginActive() const
{
    if (m_w)
    {
        return m_w->pluginActive();
    }
    return false;
}

/*! save Rule State (timesTriggered, lastTriggered) in DB only if
 *  no Button was pressed for 3 seconds.
 */
void DeRestPluginPrivate::saveCurrentRuleInDbTimerFired()
{
    queSaveDb(DB_RULES , DB_SHORT_SAVE_DELAY);
}

/*! Checks if some tcp connections could be closed.
 */
void DeRestPluginPrivate::openClientTimerFired()
{
    std::list<TcpClient>::iterator i = openClients.begin();
    std::list<TcpClient>::iterator end = openClients.end();

    for ( ; i != end; ++i)
    {
        i->closeTimeout--;
        if (i->closeTimeout == 0)
        {
            i->closeTimeout = -1;

            DBG_Assert(i->sock != 0);

            if (i->sock)
            {
                QTcpSocket *sock = i->sock;

                if (sock->state() == QTcpSocket::ConnectedState)
                {
                    DBG_Printf(DBG_INFO_L2, "Close socket port: %u\n", sock->peerPort());
                    sock->close();
                }
                else
                {
                    DBG_Printf(DBG_INFO_L2, "Close socket state = %d\n", sock->state());
                }

                sock->deleteLater();
                return;
            }
        }
    }
}

/*! Is called before the client socket will be deleted.
 */
void DeRestPluginPrivate::clientSocketDestroyed()
{
    QObject *obj = sender();
    QTcpSocket *sock = static_cast<QTcpSocket *>(obj);

    std::list<TcpClient>::iterator i = openClients.begin();
    std::list<TcpClient>::iterator end = openClients.end();

    for ( ; i != end; ++i)
    {
        if (i->sock == sock)
        {
            openClients.erase(i);
            return;
        }
    }
}

/*! Returns the endpoint number of the HA endpoint.
    \return 1..254 - on success
            0 - if not found
 */
uint8_t DeRestPluginPrivate::endpoint()
{
    if (haEndpoint != 0)
    {
        return haEndpoint;
    }

    const deCONZ::Node *node;

    if (apsCtrl && apsCtrl->getNode(0, &node) == 0)
    {
        std::vector<uint8_t> eps = node->endpoints();

        std::vector<uint8_t>::const_iterator i = eps.begin();
        std::vector<uint8_t>::const_iterator end = eps.end();

        for (; i != end; ++i)
        {
            deCONZ::SimpleDescriptor sd;
            if (node->copySimpleDescriptor(*i, &sd) == 0)
            {
                if (sd.profileId() == HA_PROFILE_ID)
                {
                    haEndpoint = sd.endpoint();
                    return haEndpoint;
                }
            }
        }
    }

    return 0;
}

/*! Returns the name of this plugin.
 */
const char *DeRestPlugin::name()
{
    return "REST API Plugin";
}

#if QT_VERSION < 0x050000
Q_EXPORT_PLUGIN2(de_rest_plugin, DeRestPlugin)
#endif
