// ClearSync: route watch plugin.
// Copyright (C) 2011 ClearFoundation <http://www.clearfoundation.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/socket.h>
#include <sys/time.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <clearsync/csplugin.h>
#include <clearsync/csnetlink.h>

#define _DEFAULT_DELAY          5

struct TableConfig_t
{
    int table;
    time_t delay;
    string *action;
    csTimer *timer;
};

class csPluginXmlParser : public csXmlParser
{
public:
    virtual void ParseElementOpen(csXmlTag *tag);
    virtual void ParseElementClose(csXmlTag *tag);
};

class csPluginRouteWatch;
class csPluginConf : public csConf
{
public:
    csPluginConf(csPluginRouteWatch *parent,
        const char *filename, csPluginXmlParser *parser)
        : csConf(filename, parser), parent(parent) { };

    virtual void Reload(void);

protected:
    friend class csPluginXmlParser;

    csPluginRouteWatch *parent;
};

void csPluginConf::Reload(void)
{
    csConf::Reload();
    parser->Parse();
}

class csPluginRouteWatch : public csPlugin
{
public:
    csPluginRouteWatch(const string &name,
        csEventClient *parent, size_t stack_size);
    virtual ~csPluginRouteWatch();

    virtual void SetConfigurationFile(const string &conf_filename);

    virtual void *Entry(void);

protected:
    friend class csPluginXmlParser;

    void QueueDelayedAction(struct TableConfig_t *config);
    void ProcessNetlinkEvent(struct nlmsghdr *nh);

    csPluginConf *conf;
    map<int, struct TableConfig_t *> table;

    size_t nl_bufsize;
    uint8_t *nl_buffer;
    csEventNetlink *nl_event;
};

csPluginRouteWatch::csPluginRouteWatch(const string &name,
    csEventClient *parent, size_t stack_size)
    : csPlugin(name, parent, stack_size), conf(NULL),
    nl_event(NULL)
{
    nl_bufsize = ::csGetPageSize() * 2;
    nl_buffer = new uint8_t[nl_bufsize];
    nl_event = new csEventNetlink(csEventNetlink::NL_RouteWatch);

    csLog::Log(csLog::Debug, "%s: Initialized.", name.c_str());
}

csPluginRouteWatch::~csPluginRouteWatch()
{
    Join();

    if (conf) delete conf;
    for (map<int, struct TableConfig_t *>::iterator i = table.begin();
        i != table.end(); i++) {
        if (i->second->timer != NULL)
            delete i->second->timer;
        if (i->second->action != NULL)
            delete i->second->action;
        delete i->second;
    }

    if (nl_buffer) delete [] nl_buffer;
    if (nl_event) delete nl_event;
}

void csPluginRouteWatch::SetConfigurationFile(const string &conf_filename)
{
    if (conf == NULL) {
        csPluginXmlParser *parser = new csPluginXmlParser();
        conf = new csPluginConf(this, conf_filename.c_str(), parser);
        parser->SetConf(dynamic_cast<csConf *>(conf));
        conf->Reload();
    }
}

void *csPluginRouteWatch::Entry(void)
{
    EventDispatch(nl_event, csThreadNetlink::GetInstance());

    for ( ;; ) {
        csEvent *event = EventPopWait();
        if (event == NULL) continue;

        switch (event->GetId()) {
        case csEVENT_QUIT:
            delete event;
            return NULL;

        case csEVENT_NETLINK:
        {
            csEventNetlink *event_netlink = static_cast<csEventNetlink *>(event);
            struct nlmsghdr *nh = event_netlink->GetReply();
            if (nh == NULL)
                csLog::Log(csLog::Error, "%s: Received NULL netlink reply!", name.c_str());
            else {
                ProcessNetlinkEvent(nh);
                delete [] (uint8_t *)nh;
            }
            break;
        }

        case csEVENT_TIMER:
        {
            int rc;
            sigset_t signal_set;
            csTimer *timer = NULL;

            if ((timer = static_cast<csEventTimer *>(event)->GetTimer()) == NULL) {
                csLog::Log(csLog::Debug, "%s: Event from NULL timer!",
                    name.c_str());
                break;
            }

            map<int, struct TableConfig_t *>::iterator i;
            i = table.find((int)timer->GetId());

            if (i != table.end()) {
                csLog::Log(csLog::Debug, "%s: Executing route watch action: %s",
                    name.c_str(), i->second->action->c_str());

                sigemptyset(&signal_set);
                sigaddset(&signal_set, SIGCHLD);
                if ((rc = pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL)) != 0) {
                    csLog::Log(csLog::Error, "%s: pthread_sigmask: %s",
                        name.c_str(), strerror(rc));

                    return NULL;
                }

                ::csExecute(*(i->second->action));

                sigemptyset(&signal_set);
                sigaddset(&signal_set, SIGCHLD);
                if ((rc = pthread_sigmask(SIG_BLOCK, &signal_set, NULL)) != 0) {
                    csLog::Log(csLog::Error, "%s: pthread_sigmask: %s",
                        name.c_str(), strerror(rc));

                    return NULL;
                }
           
                delete i->second->timer; 
                i->second->timer = NULL;
            }

            delete event;
            break;
        }

        default:
            delete event;
        }
    }

    return NULL;
}

void csPluginRouteWatch::ProcessNetlinkEvent(struct nlmsghdr *nh)
{
    struct rtmsg *rth;

    if (nh->nlmsg_type == NLMSG_DONE) {
        csLog::Log(csLog::Debug, "%s: End of multi-part message",
            name.c_str());
        return;
    }

    if (nh->nlmsg_type == NLMSG_ERROR) {
        csLog::Log(csLog::Error, "%s: NLMSG_ERROR",
            name.c_str());
        return;
    }

    if (nh->nlmsg_type == NLMSG_OVERRUN) {
        csLog::Log(csLog::Error, "%s: NLMSG_OVERRUN",
            name.c_str());
        return;
    }

    if (nh->nlmsg_type != RTM_NEWROUTE &&
        nh->nlmsg_type != RTM_DELROUTE) {
        csLog::Log(csLog::Warning, "%s: Unexpected netlink message type: %d",
            name.c_str(), nh->nlmsg_type);
        return;
    }

    rth = (struct rtmsg *)NLMSG_DATA(nh);
    map<int, struct TableConfig_t *>::iterator i = table.find(rth->rtm_table);
    if (i == table.end()) {
        csLog::Log(csLog::Debug, "%s: Event for other routing table: %d",
            name.c_str(), rth->rtm_table);
        return;
    }

    if (rth->rtm_family != AF_INET &&
        rth->rtm_family != AF_INET6) {
        csLog::Log(csLog::Debug, "%s: Event for other protocol family: %d",
            name.c_str(), rth->rtm_family);
        return;
    }

    if (rth->rtm_family == AF_INET6) {
        csLog::Log(csLog::Debug, "%s: No IPv6 support (yet)",
            name.c_str());
        return;
    }

    switch (nh->nlmsg_type) {
    case RTM_NEWROUTE:
        csLog::Log(csLog::Debug, "%s: New route", name.c_str());
        QueueDelayedAction(i->second);
        break;
    case RTM_DELROUTE:
        csLog::Log(csLog::Debug, "%s: Deleted route", name.c_str());
        QueueDelayedAction(i->second);
        break;
    default:
        csLog::Log(csLog::Debug, "%s: Received message: %d",
            name.c_str(), nh->nlmsg_type);
        break;
    }
}

void csPluginRouteWatch::QueueDelayedAction(struct TableConfig_t *config)
{
    if (config->timer)
        config->timer->SetValue(config->delay);
    else {
        config->timer = new csTimer(
            (cstimer_id_t)config->table, config->delay, 0, this);
        config->timer->Start();
    }
}

void csPluginXmlParser::ParseElementOpen(csXmlTag *tag)
{
    csPluginConf *_conf = static_cast<csPluginConf *>(conf);

    if ((*tag) == "on-route-change") {
        if (!stack.size() || (*stack.back()) != "plugin")
            ParseError("unexpected tag: " + tag->GetName());
        if (!tag->ParamExists("table"))
            ParseError("parameter missing: " + tag->GetName());

        time_t delay = _DEFAULT_DELAY;
        if (tag->ParamExists("delay"))
            delay = (time_t)atoi(tag->GetParamValue("delay").c_str());

        struct TableConfig_t *config = new struct TableConfig_t;
        config->table = atoi(tag->GetParamValue("table").c_str());
        config->timer = NULL;
        config->delay = delay;
        config->action = NULL;

        tag->SetData((void *)config);

        csLog::Log(csLog::Debug,
            "%s: Watching routing table %d for changes.",
            _conf->parent->name.c_str(), config->table);
    }
}

void csPluginXmlParser::ParseElementClose(csXmlTag *tag)
{
    string text = tag->GetText();
    csPluginConf *_conf = static_cast<csPluginConf *>(conf);

    if ((*tag) == "on-route-change") {
        if (!stack.size() || (*stack.back()) != "plugin")
            ParseError("unexpected tag: " + tag->GetName());
        if (!text.size())
            ParseError("missing value for tag: " + tag->GetName());

        csLog::Log(csLog::Debug, "%s: %s: %s",
            _conf->parent->name.c_str(),
            tag->GetName().c_str(), text.c_str());

        struct TableConfig_t *config = (struct TableConfig_t *)tag->GetData();
        config->action = new string(text);
        _conf->parent->table[config->table] = config;
    }
}

csPluginInit(csPluginRouteWatch);

// vi: expandtab shiftwidth=4 softtabstop=4 tabstop=4
