/*
 * Copyright (c) 2016 dresden elektronik ingenieurtechnik gmbh.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

#include <QString>
#include <QVariantMap>
#include <QRegExp>
#include <QStringBuilder>
#include "de_web_plugin.h"
#include "de_web_plugin_private.h"
#include "json.h"

#define MAX_RULES_COUNT 500


/*! Rules REST API broker.
    \param req - request data
    \param rsp - response data
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::handleRulesApi(const ApiRequest &req, ApiResponse &rsp)
{
    if(!checkApikeyAuthentification(req, rsp))
    {
        return REQ_READY_SEND;
    }

    // GET /api/<apikey>/rules
    if ((req.path.size() == 3) && (req.hdr.method() == "GET")  && (req.path[2] == "rules"))
    {
        return getAllRules(req, rsp);
    }
    // GET /api/<apikey>/rules/<id>
    else if ((req.path.size() == 4) && (req.hdr.method() == "GET") && (req.path[2] == "rules"))
    {
        return getRule(req, rsp);
    }
    // POST /api/<apikey>/rules
    else if ((req.path.size() == 3) && (req.hdr.method() == "POST") && (req.path[2] == "rules"))
    {
        return createRule(req, rsp);
    }
    // PUT /api/<apikey>/rules/<id>
    else if ((req.path.size() == 4) && (req.hdr.method() == "PUT") && (req.path[2] == "rules"))
    {
        return updateRule(req, rsp);
    }
    // DELETE /api/<apikey>/rules/<id>
    else if ((req.path.size() == 4) && (req.hdr.method() == "DELETE") && (req.path[2] == "rules"))
    {
        return deleteRule(req, rsp);
    }

    return REQ_NOT_HANDLED;
}


/*! GET /api/<apikey>/rules
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::getAllRules(const ApiRequest &req, ApiResponse &rsp)
{
    Q_UNUSED(req);
    rsp.httpStatus = HttpStatusOk;

    std::vector<Rule>::const_iterator i = rules.begin();
    std::vector<Rule>::const_iterator end = rules.end();

    for (; i != end; ++i)
    {
        // ignore deleted rules
        if (i->state() == Rule::StateDeleted)
        {
            continue;
        }

        QVariantMap rule;

        std::vector<RuleCondition>::const_iterator c = i->conditions().begin();
        std::vector<RuleCondition>::const_iterator cend = i->conditions().end();

        QVariantList conditions;

        for (; c != cend; ++c)
        {
            QVariantMap condition;
            condition["address"] = c->address();
            condition["operator"] = c->ooperator();
            if (c->value() != "" )
            {
                condition["value"] = c->value();
            }
            conditions.append(condition);
        }

        std::vector<RuleAction>::const_iterator a = i->actions().begin();
        std::vector<RuleAction>::const_iterator aend = i->actions().end();

        QVariantList actions;

        for (; a != aend; ++a)
        {
            QVariantMap action;
            action["address"] = a->address();
            action["method"] = a->method();

            //parse body
            bool ok;
            QVariant body = Json::parse(a->body(), ok);

            if (ok)
            {
                action["body"] = body;
                actions.append(action);
            }
        }

        rule["name"] = i->name();
        rule["lasttriggered"] = i->lastTriggered();
        rule["created"] = i->creationtime();
        rule["timestriggered"] = i->timesTriggered();
        rule["owner"] = i->owner();
        rule["status"] = i->status();
        rule["conditions"] = conditions;
        rule["actions"] = actions;
        rule["periodic"] = (double)i->triggerPeriodic();

        QString etag = i->etag;
        etag.remove('"'); // no quotes allowed in string
        rule["etag"] = etag;

        rsp.map[i->id()] = rule;
    }

    if (rsp.map.isEmpty())
    {
        rsp.str = "{}"; // return empty object
    }

    return REQ_READY_SEND;
}


/*! GET /api/<apikey>/rules/<id>
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::getRule(const ApiRequest &req, ApiResponse &rsp)
{
    DBG_Assert(req.path.size() == 4);

    if (req.path.size() != 4)
    {
        return -1;
    }

    const QString &id = req.path[3];

    Rule *rule = getRuleForId(id);

    if (!rule || (rule->state() == Rule::StateDeleted))
    {
        rsp.list.append(errorToMap(ERR_RESOURCE_NOT_AVAILABLE, QString("/rules/%1").arg(id), QString("resource, /rules/%1, not available").arg(id)));
        rsp.httpStatus = HttpStatusNotFound;
        return REQ_READY_SEND;
    }

    std::vector<RuleCondition>::const_iterator c = rule->conditions().begin();
    std::vector<RuleCondition>::const_iterator c_end = rule->conditions().end();

    QVariantList conditions;

    for (; c != c_end; ++c)
    {
        QVariantMap condition;
        condition["address"] = c->address();
        condition["operator"] = c->ooperator();
        if (c->value() != "" )
        {
            condition["value"] = c->value();
        }
        conditions.append(condition);
    }

    std::vector<RuleAction>::const_iterator a = rule->actions().begin();
    std::vector<RuleAction>::const_iterator a_end = rule->actions().end();

    QVariantList actions;

    for (; a != a_end; ++a)
    {
        QVariantMap action;
        action["address"] = a->address();
        action["method"] = a->method();

        //parse body
        bool ok;
        QVariant body = Json::parse(a->body(), ok);
        QVariantMap bodymap = body.toMap();

        QVariantMap::const_iterator b = bodymap.begin();
        QVariantMap::const_iterator b_end = bodymap.end();

        QVariantMap resultmap;

        for (; b != b_end; ++b)
        {
            resultmap[b.key()] = b.value();
        }

        action["body"] = resultmap;
        actions.append(action);
    }

    rsp.map["name"] = rule->name();
    rsp.map["lasttriggered"] = rule->lastTriggered();
    rsp.map["created"] = rule->creationtime();
    rsp.map["timestriggered"] = rule->timesTriggered();
    rsp.map["owner"] = rule->owner();
    rsp.map["status"] = rule->status();
    rsp.map["conditions"] = conditions;
    rsp.map["actions"] = actions;
    rsp.map["periodic"] = (double)rule->triggerPeriodic();

    QString etag = rule->etag;
    etag.remove('"'); // no quotes allowed in string
    rsp.map["etag"] = etag;

    rsp.httpStatus = HttpStatusOk;

    return REQ_READY_SEND;
}


/*! POST /api/<apikey>/rules
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::createRule(const ApiRequest &req, ApiResponse &rsp)
{
    bool error = false;

    rsp.httpStatus = HttpStatusOk;
    const QString &apikey = req.path[1];

    bool ok;
    Rule rule;
    QVariant var = Json::parse(req.content, ok);
    QVariantMap map = var.toMap();
    QVariantList conditionsList = map["conditions"].toList();
    QVariantList actionsList = map["actions"].toList();

    if (!ok)
    {
        rsp.list.append(errorToMap(ERR_INVALID_JSON, QString("/rules"), QString("body contains invalid JSON")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    userActivity();
/*
    if (rules.size() >= MAX_RULES_COUNT) //deletet rules will be count
    {
        rsp.list.append(errorToMap(ERR_RULE_ENGINE_FULL , QString("/rules/"), QString("The Rule Engine has reached its maximum capacity of %1 rules").arg(MAX_RULES_COUNT)));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }
*/
    //check invalid parameter

    if (!map.contains("name"))
    {
        error = true;
        rsp.list.append(errorToMap(ERR_MISSING_PARAMETER, QString("/rules/name"), QString("invalid/missing parameters in body")));
    }

    if (conditionsList.size() < 1)
    {
        error = true;
        rsp.list.append(errorToMap(ERR_MISSING_PARAMETER, QString("/rules/conditions"), QString("invalid/missing parameters in body")));
    }

    if (actionsList.size() < 1)
    {
        error = true;
        rsp.list.append(errorToMap(ERR_MISSING_PARAMETER, QString("/rules/actions"), QString("invalid/missing parameters in body")));
    }

    if (conditionsList.size() > 8)
    {
        error = true;
        rsp.list.append(errorToMap(ERR_TOO_MANY_ITEMS, QString("/rules/conditions"), QString("too many items in list")));
    }

    if (actionsList.size() > 8)
    {
        error = true;
        rsp.list.append(errorToMap(ERR_TOO_MANY_ITEMS, QString("/rules/actions"), QString("too many items in list")));
    }

    if (map.contains("status")) // optional
    {
        QString status = map["status"].toString();
        if (!(status == "disabled" || status == "enabled"))
        {
            error = true;
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/status"), QString("invalid value, %1, for parameter, status").arg(status)));
        }
    }

    if (map.contains("periodic")) // optional
    {
        int periodic = map["periodic"].toInt(&ok);

        if (!ok)
        {
            error = true;
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/periodic"), QString("invalid value, %1, for parameter, peridoc").arg(map["periodic"].toString())));
        }
        else
        {
            rule.setTriggerPeriodic(periodic);
        }
    }

    //resolve errors
    if (error)
    {
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }
    else
    {
        QString name = map["name"].toString();

        if ((map["name"].type() == QVariant::String) && !name.isEmpty())
        {
            QVariantMap rspItem;
            QVariantMap rspItemState;

            // create a new rule id
            rule.setId("1");

            do {
                ok = true;
                std::vector<Rule>::const_iterator i = rules.begin();
                std::vector<Rule>::const_iterator end = rules.end();

                for (; i != end; ++i)
                {
                    if (i->id() == rule.id())
                    {
                        rule.setId(QString::number(i->id().toInt() + 1));
                        ok = false;
                    }
                }
            } while (!ok);

            //setName
            rule.setName(name);
            rule.setOwner(apikey);
            rule.setCreationtime(QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss"));

            //setStatus optional
            if (map.contains("status"))
            {
                rule.setStatus(map["status"].toString());
            }

            //setActions
            if (checkActions(actionsList, rsp))
            {
                std::vector<RuleAction> actions;
                QVariantList::const_iterator ai = actionsList.begin();
                QVariantList::const_iterator aend = actionsList.end();

                for (; ai != aend; ++ai)
                {
                    QVariantMap bodymap = (ai->toMap()["body"]).toMap();
                    RuleAction newAction;
                    newAction.setAddress(ai->toMap()["address"].toString());
                    newAction.setBody(Json::serialize(bodymap));
                    newAction.setMethod(ai->toMap()["method"].toString());
                    actions.push_back(newAction);
                }

                rule.setActions(actions);
            }
            else
            {
                rsp.httpStatus = HttpStatusBadRequest;
                return REQ_READY_SEND;
            }

            //setConditions
            if (checkConditions(conditionsList, rsp))
            {
                std::vector<RuleCondition> conditions;
                QVariantList::const_iterator ci = conditionsList.begin();
                QVariantList::const_iterator cend = conditionsList.end();

                for (; ci != cend; ++ci)
                {
                    RuleCondition newCondition;
                    newCondition.setAddress(ci->toMap()["address"].toString());
                    newCondition.setOperator(ci->toMap()["operator"].toString());
                    newCondition.setValue(ci->toMap()["value"].toString());
                    conditions.push_back(newCondition);
                }

                rule.setConditions(conditions);
            }
            else
            {
                rsp.httpStatus = HttpStatusBadRequest;
                return REQ_READY_SEND;
            }

            updateEtag(rule.etag);
            updateEtag(gwConfigEtag);

            {
                bool found = false;
                std::vector<Rule>::iterator ri = rules.begin();
                std::vector<Rule>::iterator rend = rules.end();
                for (; ri != rend; ++ri)
                {
                    if (ri->actions() == rule.actions() &&
                        ri->conditions() == rule.conditions())
                    {
                        DBG_Printf(DBG_INFO, "replace existing rule with newly created one\n");
                        found = true;
                        *ri = rule;
                        queueCheckRuleBindings(*ri);
                        break;
                    }
                }

                if (!found)
                {
                    rules.push_back(rule);
                    queueCheckRuleBindings(rule);
                }
            }
            queSaveDb(DB_RULES, DB_SHORT_SAVE_DELAY);

            rspItemState["id"] = rule.id();
            rspItem["success"] = rspItemState;
            rsp.list.append(rspItem);
            rsp.httpStatus = HttpStatusOk;
            return REQ_READY_SEND;
        }
        else
        {
            rsp.list.append(errorToMap(ERR_INVALID_JSON, QString("/rules"), QString("body contains invalid JSON")));
            rsp.httpStatus = HttpStatusBadRequest;
        }
    }

    return REQ_READY_SEND;
}


/*! PUT /api/<apikey>/rules/<id>
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::updateRule(const ApiRequest &req, ApiResponse &rsp)
{
    bool ok;
    bool error = false;
    bool changed = false;

    QString id = req.path[3];

    QVariant var = Json::parse(req.content, ok);
    QVariantMap map = var.toMap();
    QVariantList conditionsList;
    QVariantList actionsList;

    QString name;
    QString status;
    int periodic = 0;

    rsp.httpStatus = HttpStatusOk;

    if (!ok)
    {
        rsp.list.append(errorToMap(ERR_INVALID_JSON, QString("/rules"), QString("body contains invalid JSON")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    userActivity();

    //check invalid parameter
    QVariantMap::const_iterator pi = map.begin();
    QVariantMap::const_iterator pend = map.end();

    for (; pi != pend; ++pi)
    {
        if(!((pi.key() == QLatin1String("name")) || (pi.key() == QLatin1String("status")) || (pi.key() == QLatin1String("actions")) || (pi.key() == QLatin1String("conditions")) || (pi.key() == QLatin1String("periodic"))))
        {
            rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/rules/%1/%2").arg(id).arg(pi.key()), QString("parameter, %1, not available").arg(pi.key())));
            rsp.httpStatus = HttpStatusBadRequest;
            return REQ_READY_SEND;
        }
    }

    if (map.contains("name")) // optional
    {
        name = map["name"].toString();

        if ((map["name"].type() == QVariant::String) && !(name.isEmpty()))
        {
            if (name.size() > MAX_RULE_NAME_LENGTH)
            {
                error = true;
                rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/%1/name").arg(id), QString("invalid value, %1, for parameter, /rules/%2/name").arg(name).arg(id)));
                rsp.httpStatus = HttpStatusBadRequest;
                name = QString();
            }
        }
        else
        {
            error = true;
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/%1/name").arg(id), QString("invalid value, %1, for parameter, /rules/%2/name").arg(name).arg(id)));
            rsp.httpStatus = HttpStatusBadRequest;
            name = QString();
        }
    }

    if (map.contains("conditions")) //optional
    {
        conditionsList = map["conditions"].toList();
        if (conditionsList.size() < 1)
        {
            error = true;
            rsp.list.append(errorToMap(ERR_MISSING_PARAMETER, QString("/rules/conditions"), QString("invalid/missing parameters in body")));
        }

        if (conditionsList.size() > 8)
        {
            error = true;
            rsp.list.append(errorToMap(ERR_TOO_MANY_ITEMS, QString("/rules/conditions"), QString("too many items in list")));
        }
    }

    if (map.contains("actions")) //optional
    {
        actionsList = map["actions"].toList();
        if (actionsList.size() < 1)
        {
            error = true;
            rsp.list.append(errorToMap(ERR_MISSING_PARAMETER, QString("/rules/actions"), QString("invalid/missing parameters in body")));
        }

        if (actionsList.size() > 8)
        {
            error = true;
            rsp.list.append(errorToMap(ERR_TOO_MANY_ITEMS, QString("/rules/actions"), QString("too many items in list")));
        }
    }
    if (map.contains("status")) // optional
    {
        status = map["status"].toString();
        if (!(status == "disabled" || status == "enabled"))
        {
            error = true;
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/status"), QString("invalid value, %1, for parameter, status").arg(status)));
        }
    }

    if (map.contains("periodic")) // optional
    {
        periodic = map["periodic"].toInt(&ok);

        if (!ok)
        {
            error = true;
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/periodic"), QString("invalid value, %1, for parameter, peridoc").arg(map["periodic"].toString())));
        }
    }

    //resolve errors
    if (error)
    {
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    std::vector<Rule>::iterator i = rules.begin();
    std::vector<Rule>::iterator end = rules.end();

    for (; i != end; ++i)
    {
        if (i->state() != Rule::StateNormal)
        {
            continue;
        }

        if (i->id() == id)
        {
            // first delete old binding if present then create new binding with updated rule
            if (map.contains("actions") || map.contains("conditions"))
            {
                i->setStatus("disabled");
                queueCheckRuleBindings(*i);
            }

            //setName optional
            if (!name.isEmpty())
            {
                QVariantMap rspItem;
                QVariantMap rspItemState;
                rspItemState[QString("/rules/%1/name").arg(id)] = name;
                rspItem["success"] = rspItemState;
                rsp.list.append(rspItem);
                if (i->name() != name)
                {
                    changed = true;
                    i->setName(name);
                }
            }

            //setStatus optional
            if (map.contains("status"))
            {
                QVariantMap rspItem;
                QVariantMap rspItemState;
                rspItemState[QString("/rules/%1/status").arg(id)] = status;
                rspItem["success"] = rspItemState;
                rsp.list.append(rspItem);
                if (i->status() != status)
                {
                    changed = true;
                    i->setStatus(status);
                }
            }

            // periodic optional
            if (map.contains("periodic"))
            {
                if (i->triggerPeriodic() != periodic)
                {
                    changed = true;
                    i->setTriggerPeriodic(periodic);
                }
            }

            //setActions optional
            if (map.contains("actions"))
            {
                changed = true;
                if (checkActions(actionsList,rsp))
                {
                    std::vector<RuleAction> actions;
                    QVariantList::const_iterator ai = actionsList.begin();
                    QVariantList::const_iterator aend = actionsList.end();

                    for (; ai != aend; ++ai)
                    {
                        RuleAction newAction;
                        newAction.setAddress(ai->toMap()["address"].toString());
                        newAction.setBody(Json::serialize(ai->toMap()["body"].toMap()));
                        newAction.setMethod(ai->toMap()["method"].toString());
                        actions.push_back(newAction);
                    }
                    i->setActions(actions);

                    QVariantMap rspItem;
                    QVariantMap rspItemState;
                    rspItemState[QString("/rules/%1/actions").arg(id)] = actionsList;
                    rspItem["success"] = rspItemState;
                    rsp.list.append(rspItem);
                }
                else
                {
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }

            //setConditions optional
            if (map.contains("conditions"))
            {
                changed = true;
                if (checkConditions(conditionsList, rsp))
                {
                    std::vector<RuleCondition> conditions;
                    QVariantList::const_iterator ci = conditionsList.begin();
                    QVariantList::const_iterator cend = conditionsList.end();

                    for (; ci != cend; ++ci)
                    {
                        RuleCondition newCondition;
                        newCondition.setAddress(ci->toMap()["address"].toString());
                        newCondition.setOperator(ci->toMap()["operator"].toString());
                        newCondition.setValue(ci->toMap()["value"].toString());
                        conditions.push_back(newCondition);

                    }
                    i->setConditions(conditions);

                    QVariantMap rspItem;
                    QVariantMap rspItemState;
                    rspItemState[QString("/rules/%1/conditions").arg(id)] = conditionsList;
                    rspItem["success"] = rspItemState;
                    rsp.list.append(rspItem);
                }
                else
                {
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }

            if (!map.contains("status"))
            {
                i->setStatus("enabled");
            }
            DBG_Printf(DBG_INFO, "force verify of rule %s: %s\n", qPrintable(i->id()), qPrintable(i->name()));
            i->lastVerify = 0;
            verifyRulesTimer->start(500);

            if (changed)
            {
                updateEtag(i->etag);
                updateEtag(gwConfigEtag);
                queSaveDb(DB_RULES, DB_SHORT_SAVE_DELAY);
            }

            break;
        }
    }

    return REQ_READY_SEND;
}


/*! Rule actions contain errors or multiple actions with the same resource address.
    \param actionsList the actionsList
 */
bool DeRestPluginPrivate::checkActions(QVariantList actionsList, ApiResponse &rsp)
{
    QVariantList::const_iterator ai = actionsList.begin();
    QVariantList::const_iterator aend = actionsList.end();

    QList<QString> addresses;

    for (; ai != aend; ++ai)
    {
        QString address = (ai->toMap()["address"]).toString();
        QString method = (ai->toMap()["method"]).toString();
        QString body = (ai->toMap()["body"]).toString();

        //check addresses
        //address must begin with / and a valid resource
        //no dublicate addresses allowed
        if (!((address.indexOf(QLatin1String("/lights")) == 0) || (address.indexOf(QLatin1String("/groups")) == 0) || (address.indexOf(QLatin1String("/scenes")) == 0) ||
            (address.indexOf(QLatin1String("/schedules")) == 0) || (address.indexOf(QLatin1String("/sensors")) == 0)) || (addresses.contains(address)))
        {
            rsp.list.append(errorToMap(ERR_ACTION_ERROR, QString(address),
                QString("Rule actions contain errors or multiple actions with the same resource address or an action on a unsupported resource")));
            return false;
        }
        else
        {
            addresses.append(address);
        }

        //check methods
        if(!((method == QLatin1String("PUT")) || (method == QLatin1String("POST")) || (method == QLatin1String("DELETE")) || (method == QLatin1String("BIND"))))
        {
            rsp.list.append(errorToMap(ERR_INVALID_VALUE , QString("rules/method"), QString("invalid value, %1, for parameter, method").arg(method)));
            return false;
        }

        //check body
        bool ok;
        Json::parse(body, ok);
        if (!ok)
        {
            rsp.list.append(errorToMap(ERR_INVALID_JSON, QString("/rules/"), QString("body contains invalid JSON")));
            return false;
        }
    }

    return true;
}


/*! Rule conditions contain errors or operator combination is not allowed.
    \param conditionsList the conditionsList
 */
bool DeRestPluginPrivate::checkConditions(QVariantList conditionsList, ApiResponse &rsp)
{
    // get valid and present sensor resources
    QVector<QString> validAddresses;
    QVector<QString> validOperators;
    QString validValues = "";

    std::vector<Sensor>::const_iterator si = sensors.begin();
    std::vector<Sensor>::const_iterator send = sensors.end();

    for (; si != send; ++si)
    {
        QString base(QLatin1String("/sensors/") + si->id());
        const QString &type = si->type();

        validAddresses.push_back(base + QLatin1String("/config/reachable"));
        validAddresses.push_back(base + QLatin1String("/config/on"));
        validAddresses.push_back(base + QLatin1String("/config/battery"));
        validAddresses.push_back(base + QLatin1String("/state/lastupdated"));

        if (type == QLatin1String("ZGPSwitch"))
        {
            validAddresses.push_back(base + QLatin1String("/state/buttonevent"));
        }
        else if (type == QLatin1String("ZHASwitch"))
        {
            validAddresses.push_back(base + QLatin1String("/state/buttonevent"));
        }
        else if (type == QLatin1String("ZHALight"))
        {
            validAddresses.push_back(base + QLatin1String("/state/illuminance"));
        }
        else if (type == QLatin1String("ZHAPresence"))
        {
            validAddresses.push_back(base + QLatin1String("/state/presence"));
        }
        else if (type == QLatin1String("CLIPOpenClose"))
        {
            validAddresses.push_back(base + QLatin1String("/state/open"));
        }
        else if (type == QLatin1String("CLIPPresence"))
        {
            validAddresses.push_back(base + QLatin1String("/state/presence"));
        }
        else if (type == QLatin1String("CLIPTemperature"))
        {
            validAddresses.push_back(base + QLatin1String("/state/temperature"));
        }
        else if (type == QLatin1String("CLIPHumidity"))
        {
            validAddresses.push_back(base + QLatin1String("/state/humidity"));
        }
        else if (type == QLatin1String("DaylightSensor"))
        {
            validAddresses.push_back(base + QLatin1String("/state/daylight"));
            validAddresses.push_back(base + QLatin1String("/config/long"));
            validAddresses.push_back(base + QLatin1String("/config/lat"));
            validAddresses.push_back(base + QLatin1String("/config/sunriseoffset"));
            validAddresses.push_back(base + QLatin1String("/config/sunsetoffset"));
        }
        else if (type == QLatin1String("CLIPGenericFlag"))
        {
            validAddresses.push_back(base + QLatin1String("/state/flag"));
        }
        else if (type == QLatin1String("CLIPGenericStatus"))
        {
            validAddresses.push_back(base + QLatin1String("/state/status"));
        }
    }

    // check condition parameters
    QVariantList::const_iterator ci = conditionsList.begin();
    QVariantList::const_iterator cend = conditionsList.end();

    for (; ci != cend; ++ci)
    {
        QString address = (ci->toMap()["address"]).toString();
        QString ooperator = (ci->toMap()["operator"]).toString();
        QString value = (ci->toMap()["value"]).toString();

        QString confstate = "";
        if (address.contains(QLatin1String("/config")))
        {
            confstate = address.mid(address.indexOf(QLatin1String("/config")));
        }
        else if (address.contains(QLatin1String("/state")))
        {
            confstate = address.mid(address.indexOf(QLatin1String("/state")));
        }

        // check address: whole address must be a valid and present resource
        if (!validAddresses.contains(address))
        {
            rsp.list.append(errorToMap(ERR_RESOURCE_NOT_AVAILABLE, QString(address),
                QString("Resource, %1, not available").arg(address)));
            return false;
        }

        //check operator in dependence of config and state of sensortype
        if ((confstate == QLatin1String("/state/lastupdated")) || (confstate == QLatin1String("/state/long")) || (confstate == QLatin1String("/state/lat")))
        {
            validOperators.push_back(QLatin1String("dx"));
        }
        else if ((confstate == QLatin1String("/state/illuminance")) || (confstate == QLatin1String("/state/presence")))
        {
            validOperators.push_back(QLatin1String("dx"));
            validOperators.push_back(QLatin1String("eq"));
            validOperators.push_back(QLatin1String("lt"));
            validOperators.push_back(QLatin1String("gt"));
            validValues = QLatin1String("numbers");
        }
        else if ((confstate == QLatin1String("/config/reachable")) || (confstate == QLatin1String("/config/on")) || (confstate == QLatin1String("/state/open"))
            || (confstate == QLatin1String("/state/presence")) || (confstate == QLatin1String("/state/flag")) || (confstate == QLatin1String("/state/daylight")))
        {
            validOperators.push_back(QLatin1String("dx"));
            validOperators.push_back(QLatin1String("eq"));
            validValues = QLatin1String("boolean");
        }
        else if ((confstate == QLatin1String("/config/battery")) || (confstate == QLatin1String("/state/buttonevent")) || (confstate == QLatin1String("/state/temperature"))
             || (confstate == QLatin1String("/state/humidity")))
        {
            validOperators.push_back(QLatin1String("dx"));
            validOperators.push_back(QLatin1String("eq"));
            validOperators.push_back(QLatin1String("gt"));
            validOperators.push_back(QLatin1String("lt"));
            validValues = QLatin1String("numbers");
        }
        else if ((confstate == QLatin1String("/config/sunriseoffset")) || (confstate == QLatin1String("/config/sunsetoffset")))
        {
            validOperators.push_back(QLatin1String("eq"));
            validOperators.push_back(QLatin1String("gt"));
            validOperators.push_back(QLatin1String("lt"));
            validValues = QLatin1String("numbers");
        }

        if (!validOperators.contains(ooperator))
        {
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/operator"), QString("invalid value, %1, for parameter, operator").arg(ooperator)));
            return false;
        }


        //check value in dependence of config and state of sensortype
        QRegExp numbers("^[1-9]\\d*$");
        QRegExp boolean("^(true|false)$");
        //QRegExp timestamp("^\\d{4,4}-\\d{2,2}-\\d{2,2}T\\d{2,2}:\\d{2,2}:\\d{2,2}.*$");
        if (ooperator == QLatin1String("dx"))
        {
            //no value allowed
            if (value != QLatin1String(""))
            {
                rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/conditions"), QString("parameter, value, is not modifiable")));
                return false;
            }
        }
        else if (validValues == QLatin1String("numbers"))
        {
                if (!value.contains(numbers))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/conditions"), QString("invalid value, %1, for parameter, value").arg(value)));
                    return false;
                }
        }
        else if (validValues == QLatin1String("boolean"))
        {
                if (!value.contains(boolean))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/rules/conditions"), QString("invalid value, %1, for parameter, value").arg(value)));
                    return false;
                }
        }
        validValues = "";
        validOperators.clear();
    }
    return true;
}


/*! DELETE /api/<apikey>/rules/<id>
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::deleteRule(const ApiRequest &req, ApiResponse &rsp)
{
    QString id = req.path[3];
    Rule *rule = getRuleForId(id);

    userActivity();

    if (!rule || (rule->state() == Rule::StateDeleted))
    {
        rsp.httpStatus = HttpStatusNotFound;
        rsp.list.append(errorToMap(ERR_RESOURCE_NOT_AVAILABLE, QString("/rules/%1").arg(id), QString("resource, /rules/%1, not available").arg(id)));
        return REQ_READY_SEND;
    }

    rule->setState(Rule::StateDeleted);
    rule->setStatus("disabled");
    queueCheckRuleBindings(*rule);

    QVariantMap rspItem;
    QVariantMap rspItemState;
    rspItemState["id"] = id;
    rspItem["success"] = rspItemState;
    rsp.list.append(rspItem);
    rsp.httpStatus = HttpStatusOk;

    queSaveDb(DB_RULES, DB_SHORT_SAVE_DELAY);

    updateEtag(gwConfigEtag);
    rsp.httpStatus = HttpStatusOk;

    return REQ_READY_SEND;
}

/*! Add a binding task to the queue and prevent double entries.
    \param bindingTask - the binding task
 */
void DeRestPluginPrivate::queueBindingTask(const BindingTask &bindingTask)
{
    if (!apsCtrl || apsCtrl->networkState() != deCONZ::InNetwork)
    {
        return;
    }

    const std::list<BindingTask>::const_iterator i = std::find(bindingQueue.begin(), bindingQueue.end(), bindingTask);

    if (i == bindingQueue.end())
    {
        DBG_Printf(DBG_INFO_L2, "queue binding task for 0x%016llu, cluster 0x%04X\n", bindingTask.binding.srcAddress, bindingTask.binding.clusterId);
        bindingQueue.push_back(bindingTask);
    }
    else
    {
        DBG_Printf(DBG_INFO, "discard double entry in binding queue\n");
    }
}

/*! Starts verification that the ZigBee bindings of a rule are present
    on the source device.
    \param rule the rule to verify
 */
void DeRestPluginPrivate::queueCheckRuleBindings(const Rule &rule)
{
    quint64 srcAddress = 0;
    quint8 srcEndpoint = 0;
    BindingTask bindingTask;
    bindingTask.state = BindingTask::StateCheck;
    Sensor *sensorNode = 0;

    Q_Q(DeRestPlugin);
    if (!q->pluginActive())
    {
        return;
    }

    if (rule.state() == Rule::StateNormal && rule.status() == "enabled")
    {
        bindingTask.action = BindingTask::ActionBind;
    }
    else if (rule.state() == Rule::StateDeleted || rule.status() == "disabled")
    {
        bindingTask.action = BindingTask::ActionUnbind;
    }
    else
    {
        DBG_Printf(DBG_INFO, "ignored checking of rule %s\n", qPrintable(rule.name()));
        return;
    }

    {   // search in conditions for binding srcAddress and srcEndpoint

        std::vector<RuleCondition>::const_iterator i = rule.conditions().begin();
        std::vector<RuleCondition>::const_iterator end = rule.conditions().end();

        for (; i != end; ++i)
        {
            // operator equal used to refer to srcEndpoint
            if (i->ooperator() != "eq")
            {
                continue;
            }

            QStringList srcAddressLs = i->address().split('/', QString::SkipEmptyParts);

            if (srcAddressLs.size() != 4) // /sensors/<id>/state/(buttonevent|illuminance)
            {
                continue;
            }

            if (srcAddressLs[0] != "sensors")
            {
                continue;
            }

            if (srcAddressLs[2] != "state")
            {
                continue;
            }

            if ((srcAddressLs[3] == "buttonevent") ||
                (srcAddressLs[3] == "illuminance") ||
                (srcAddressLs[3] == "presence"))
            {
                sensorNode = getSensorNodeForId(srcAddressLs[1]);

                if (sensorNode && sensorNode->isAvailable() && sensorNode->node())
                {
                    bool ok = false;
                    quint16 ep = i->value().toUShort(&ok);

                    if (ok)
                    {
                        const std::vector<quint8> &activeEndpoints = sensorNode->node()->endpoints();

                        for (uint i = 0; i < activeEndpoints.size(); i++)
                        {
                            // check valid endpoint in 'value'
                            if (ep == activeEndpoints[i])
                            {
                                srcAddress = sensorNode->address().ext();
                                srcEndpoint = ep;
                                sensorNode->enableRead(READ_BINDING_TABLE);
                                sensorNode->setNextReadTime(QTime::currentTime());
                                q->startZclAttributeTimer(1000);
                                break;
                            }
                        }

                        // found source addressing?
                        if ((srcAddress == 0) || (srcEndpoint == 0))
                        {
                            DBG_Printf(DBG_INFO, "no src addressing found for rule %s\n", qPrintable(rule.name()));
                        }
                    }
                }
                else
                {
                    void *n = 0;
                    uint avail = false;

                    if (sensorNode)
                    {
                        avail = sensorNode->isAvailable();
                        n = sensorNode->node();
                    }

                    DBG_Printf(DBG_INFO, "skip verify rule %s for sensor %s (available = %u, node = %p, sensorNode = %p)\n",
                               qPrintable(rule.name()), qPrintable(srcAddressLs[1]), avail, n, sensorNode);
                }
            }
        }
    }

    if (!sensorNode)
    {
        return;
    }


    // found source addressing?
    if ((srcAddress == 0) || (srcEndpoint == 0))
    {
        return;
    }

    bindingTask.restNode = sensorNode;

    DBG_Printf(DBG_INFO, "verify Rule %s: %s\n", qPrintable(rule.id()), qPrintable(rule.name()));

    { // search in actions for binding dstAddress, dstEndpoint and clusterId
        std::vector<RuleAction>::const_iterator i = rule.actions().begin();
        std::vector<RuleAction>::const_iterator end = rule.actions().end();

        for (; i != end; ++i)
        {
            if (i->method() != "BIND")
            {
                continue;
            }

            Binding &bnd = bindingTask.binding;
            bnd.srcAddress = srcAddress;
            bnd.srcEndpoint = srcEndpoint;
            bool ok = false;

            if (!sensorNode->config().on())
            {
                if (bindingTask.action == BindingTask::ActionBind)
                {
                    DBG_Printf(DBG_INFO, "Sensor %s is 'off', prevent Rule %s: %s activation\n", qPrintable(sensorNode->id()), qPrintable(rule.id()), qPrintable(rule.name()));
                    bindingTask.action = BindingTask::ActionUnbind;
                }
            }

            QStringList dstAddressLs = i->address().split('/', QString::SkipEmptyParts);

            // /groups/0/action
            // /lights/2/state
            if (dstAddressLs.size() == 3)
            {
                if (dstAddressLs[0] == "groups")
                {
                    bnd.dstAddress.group = dstAddressLs[1].toUShort(&ok);
                    bnd.dstAddrMode = deCONZ::ApsGroupAddress;
                }
                else if (dstAddressLs[0] == "lights")
                {
                    LightNode *lightNode = getLightNodeForId(dstAddressLs[1]);
                    if (lightNode)
                    {
                        bnd.dstAddress.ext = lightNode->address().ext();
                        bnd.dstEndpoint = lightNode->haEndpoint().endpoint();
                        bnd.dstAddrMode = deCONZ::ApsExtAddress;
                        ok = true;
                    }
                }
                else
                {
                    // unsupported addressing
                    continue;
                }

                if (!ok)
                {
                    continue;
                }

                // action.body might contain multiple 'bindings'
                // TODO check if clusterId is available (finger print?)

                if (i->body().contains("on"))
                {
                    bnd.clusterId = ONOFF_CLUSTER_ID;
                    queueBindingTask(bindingTask);
                }

                if (i->body().contains("bri"))
                {
                    bnd.clusterId = LEVEL_CLUSTER_ID;
                    queueBindingTask(bindingTask);
                }

                if (i->body().contains("scene"))
                {
                    bnd.clusterId = SCENE_CLUSTER_ID;
                    queueBindingTask(bindingTask);
                }

                if (i->body().contains("illum"))
                {
                    bnd.clusterId = ILLUMINANCE_MEASUREMENT_CLUSTER_ID;
                    queueBindingTask(bindingTask);
                }

                if (i->body().contains("occ"))
                {
                    bnd.clusterId = OCCUPANCY_SENSING_CLUSTER_ID;
                    queueBindingTask(bindingTask);
                }
            }
        }
    }

    if (!bindingTimer->isActive())
    {
        bindingTimer->start();
    }
}

/*! Triggers actions of a rule if needed.
    \param rule - the rule to check
 */
void DeRestPluginPrivate::triggerRuleIfNeeded(Rule &rule)
{
    if (!apsCtrl || (apsCtrl->networkState() != deCONZ::InNetwork))
    {
        return;
    }

    if (!(rule.state() == Rule::StateNormal && rule.status() == QLatin1String("enabled")))
    {
        return;
    }

    if (rule.triggerPeriodic() < 0)
    {
        return;
    }

    if (rule.triggerPeriodic() == 0)
    {
        // trigger on event
        // TODO implement events for rule
        return;
    }

    if (rule.triggerPeriodic() > 0)
    {
        if (rule.lastTriggeredTime().isValid() &&
            rule.lastTriggeredTime().elapsed() < rule.triggerPeriodic())
        {
            // not yet time
            return;
        }
    }

    std::vector<RuleCondition>::const_iterator ci = rule.conditions().begin();
    std::vector<RuleCondition>::const_iterator cend = rule.conditions().end();

    for (; ci != cend; ++ci)
    {
        QStringList ls = ci->address().split(QChar('/'), QString::SkipEmptyParts);

        if (ls.size() < 4) // sensors, <id>, state, (illuminance|buttonevent)
            return;

        if (ls[0] != QLatin1String("sensors"))
        {
            return;
        }

        Sensor *sensor = getSensorNodeForId(ls[1]);

        if (!sensor)
        {
            return;
        }

        if (!sensor->isAvailable())
        {
            return;
        }

        if (ls.last() == QLatin1String("buttonevent"))
        {
            return; // TODO
        }
        else if (ls.last() == QLatin1String("illuminance"))
        {
            { // check if value is fresh enough
                NodeValue &val = sensor->getZclValue(ILLUMINANCE_MEASUREMENT_CLUSTER_ID, 0x0000);

                if (!val.timestamp.isValid() ||
                     val.timestamp.elapsed() > MAX_RULE_ILLUMINANCE_VALUE_AGE_MS)
                {
                    if (val.timestampLastReadRequest.isValid() &&
                        val.timestampLastReadRequest.elapsed() < (MAX_RULE_ILLUMINANCE_VALUE_AGE_MS / 2))
                    {
                        return;
                    }

                    std::vector<quint16> attrs;
                    attrs.push_back(0x0000); // measured value
                    DBG_Printf(DBG_INFO, "force read illuminance value of 0x%016llX\n", sensor->address().ext());
                    if (readAttributes(sensor, sensor->fingerPrint().endpoint, ILLUMINANCE_MEASUREMENT_CLUSTER_ID, attrs))
                    {
                        val.timestampLastReadRequest.start();
                    }

                    return;
                }
            }

            bool ok;
            uint cval = ci->value().toUInt(&ok);

            if (!ok)
            {
                DBG_Printf(DBG_INFO, "invalid rule.condition.value %s\n", qPrintable(ci->value()));
            }

            if (ci->ooperator() == QLatin1String("lt"))
            {
                if (sensor->state().lux() >= cval)
                {
                    return; // condition not met
                }
            }
            else if (ci->ooperator() == QLatin1String("gt"))
            {
                if (sensor->state().lux() <= cval)
                {
                    return; // condition not met
                }
            }
            else
            {
                return; // unsupported condition operator
            }
        }
        else
        {
            return; // unsupported condition address
        }
    }

    // conditions ok, trigger action
    bool triggered = false;
    std::vector<RuleAction>::const_iterator ai = rule.actions().begin();
    std::vector<RuleAction>::const_iterator aend = rule.actions().end();

    for (; ai != aend; ++ai)
    {
        if (ai->method() != QLatin1String("PUT"))
            return;

        QStringList path = ai->address().split(QChar('/'), QString::SkipEmptyParts);

        if (path.size() < 3) // groups, <id>, action
            return;

        QHttpRequestHeader hdr(ai->method(), ai->address());

        // paths start with /api/<apikey/ ...>
        path.prepend(rule.owner()); // apikey
        path.prepend(QLatin1String("api")); // api

        ApiRequest req(hdr, path, NULL, ai->body());
        ApiResponse rsp; // dummy

        if (path[2] == QLatin1String("groups"))
        {
            if (handleGroupsApi(req, rsp) == REQ_NOT_HANDLED)
            {
                return;
            }
            triggered = true;
        }
        else if (path[2] == QLatin1String("lights"))
        {
            if (handleLightsApi(req, rsp) == REQ_NOT_HANDLED)
            {
                return;
            }
            triggered = true;
        }
        else
        {
            DBG_Printf(DBG_INFO, "unsupported rule action address %s\n", qPrintable(ai->address()));
            return;
        }
    }

    if (triggered)
    {
        rule.setLastTriggered(QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss"));
        rule.setTimesTriggered(rule.timesTriggered() + 1);
    }
}

/*! Verifies that rule bindings are valid. */
void DeRestPluginPrivate::verifyRuleBindingsTimerFired()
{
    if (!apsCtrl || (apsCtrl->networkState() != deCONZ::InNetwork) || rules.empty())
    {
        return;
    }

    Q_Q(DeRestPlugin);
    if (!q->pluginActive())
    {
        return;
    }

    if (verifyRuleIter >= rules.size())
    {
        verifyRuleIter = 0;
    }

    Rule &rule = rules[verifyRuleIter];

    triggerRuleIfNeeded(rule);

    if (bindingQueue.size() < 16)
    {
        if (rule.state() == Rule::StateNormal)
        {
            if ((rule.lastVerify + Rule::MaxVerifyDelay) < idleTotalCounter)
            {
                rule.lastVerify = idleTotalCounter;
                queueCheckRuleBindings(rule);
            }
        }
    }
    else
    {
        DBG_Printf(DBG_INFO, "");
    }

    verifyRuleIter++;
}
