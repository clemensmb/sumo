/****************************************************************************/
/// @file    MSDevice_Routing.cpp
/// @author  Michael Behrisch
/// @author  Daniel Krajzewicz
/// @author  Laura Bieker
/// @author  Christoph Sommer
/// @author  Jakob Erdmann
/// @date    Tue, 04 Dec 2007
/// @version $Id$
///
// A device that performs vehicle rerouting based on current edge speeds
/****************************************************************************/
// SUMO, Simulation of Urban MObility; see http://sumo-sim.org/
// Copyright (C) 2007-2014 DLR (http://www.dlr.de/) and contributors
/****************************************************************************/
//
//   This file is part of SUMO.
//   SUMO is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
/****************************************************************************/

// ===========================================================================
// included modules
// ===========================================================================
#ifdef _MSC_VER
#include <windows_config.h>
#else
#include <config.h>
#endif

#include "MSDevice_Routing.h"
#include <microsim/MSNet.h>
#include <microsim/MSLane.h>
#include <microsim/MSEdge.h>
#include <microsim/MSEdgeControl.h>
#include <utils/options/OptionsCont.h>
#include <utils/common/WrappingCommand.h>
#include <utils/common/StaticCommand.h>
#include <utils/vehicle/DijkstraRouterTT.h>
#include <utils/vehicle/AStarRouter.h>

#ifdef CHECK_MEMORY_LEAKS
#include <foreign/nvwa/debug_new.h>
#endif // CHECK_MEMORY_LEAKS


// ===========================================================================
// static member variables
// ===========================================================================
std::vector<SUMOReal> MSDevice_Routing::myEdgeEfforts;
Command* MSDevice_Routing::myEdgeWeightSettingCommand = 0;
SUMOReal MSDevice_Routing::myAdaptationWeight;
SUMOTime MSDevice_Routing::myAdaptationInterval = -1;
SUMOTime MSDevice_Routing::myLastAdaptation = -1;
bool MSDevice_Routing::myWithTaz;
std::map<std::pair<const MSEdge*, const MSEdge*>, const MSRoute*> MSDevice_Routing::myCachedRoutes;
SUMOAbstractRouter<MSEdge, SUMOVehicle>* MSDevice_Routing::myRouter = 0;
#ifdef HAVE_FOX
FXWorkerThread::Pool MSDevice_Routing::myThreadPool;
#endif


// ===========================================================================
// method definitions
// ===========================================================================
// ---------------------------------------------------------------------------
// static initialisation methods
// ---------------------------------------------------------------------------
void
MSDevice_Routing::insertOptions(OptionsCont& oc) {
    oc.addOptionSubTopic("Routing");
    insertDefaultAssignmentOptions("rerouting", "Routing", oc);

    oc.doRegister("device.rerouting.period", new Option_String("0", "TIME"));
    oc.addSynonyme("device.rerouting.period", "device.routing.period", true);
    oc.addDescription("device.rerouting.period", "Routing", "The period with which the vehicle shall be rerouted");

    oc.doRegister("device.rerouting.pre-period", new Option_String("0", "TIME"));
    oc.addSynonyme("device.rerouting.pre-period", "device.routing.pre-period", true);
    oc.addDescription("device.rerouting.pre-period", "Routing", "The rerouting period before depart");

    oc.doRegister("device.rerouting.adaptation-weight", new Option_Float(.5));
    oc.addSynonyme("device.rerouting.adaptation-weight", "device.routing.adaptation-weight", true);
    oc.addDescription("device.rerouting.adaptation-weight", "Routing", "The weight of prior edge weights");

    oc.doRegister("device.rerouting.adaptation-interval", new Option_String("1", "TIME"));
    oc.addSynonyme("device.rerouting.adaptation-interval", "device.routing.adaptation-interval", true);
    oc.addDescription("device.rerouting.adaptation-interval", "Routing", "The interval for updating the edge weights");

    oc.doRegister("device.rerouting.with-taz", new Option_Bool(false));
    oc.addSynonyme("device.rerouting.with-taz", "device.routing.with-taz", true);
    oc.addDescription("device.rerouting.with-taz", "Routing", "Use zones (districts) as routing end points");

    oc.doRegister("device.rerouting.init-with-loaded-weights", new Option_Bool(false));
    oc.addDescription("device.rerouting.init-with-loaded-weights", "Routing", "Use given weight files for initializing edge weights");

#ifdef HAVE_FOX
    oc.doRegister("device.rerouting.threads", new Option_Integer(0));
    oc.addDescription("device.rerouting.threads", "Routing", "The number of parallel execution threads used for rerouting");
#endif
    
    myEdgeWeightSettingCommand = 0;
    myEdgeEfforts.clear();
}


void
MSDevice_Routing::buildVehicleDevices(SUMOVehicle& v, std::vector<MSDevice*>& into) {
    bool needRerouting = v.getParameter().wasSet(VEHPARS_FORCE_REROUTE);
    OptionsCont& oc = OptionsCont::getOptions();
    if (!needRerouting && oc.getFloat("device.rerouting.probability") == 0 && !oc.isSet("device.rerouting.explicit")) {
        // no route computation is modelled
        return;
    }
    needRerouting |= equippedByDefaultAssignmentOptions(OptionsCont::getOptions(), "rerouting", v);
    if (needRerouting) {
        // route computation is enabled
        myWithTaz = oc.getBool("device.rerouting.with-taz");
        const SUMOTime period = string2time(oc.getString("device.rerouting.period"));
        const SUMOTime prePeriod = string2time(oc.getString("device.rerouting.pre-period"));
        // initialise edge efforts if not done before
        if (myEdgeEfforts.size() == 0) {
            const std::vector<MSEdge*>& edges = MSNet::getInstance()->getEdgeControl().getEdges();
            const bool useLoaded = oc.getBool("device.rerouting.init-with-loaded-weights");
            const SUMOReal currentSecond = SIMTIME;
            for (std::vector<MSEdge*>::const_iterator i = edges.begin(); i != edges.end(); ++i) {
                while ((*i)->getNumericalID() >= (int)myEdgeEfforts.size()) {
                    myEdgeEfforts.push_back(0);
                }
                if (useLoaded) {
                    myEdgeEfforts[(*i)->getNumericalID()] = MSNet::getTravelTime(*i, 0, currentSecond);
                } else {
                    myEdgeEfforts[(*i)->getNumericalID()] = (*i)->getCurrentTravelTime();
                }
            }
        }
        // make the weights be updated
        if (myAdaptationInterval == -1) {
            myAdaptationInterval = string2time(oc.getString("device.rerouting.adaptation-interval"));
            if (myAdaptationInterval < 0) {
                WRITE_ERROR("Negative value for device.rerouting.adaptation-interval!");
            }
            myAdaptationWeight = oc.getFloat("device.rerouting.adaptation-weight");
            if (myAdaptationWeight < 0. || myAdaptationWeight > 1.) {
                WRITE_ERROR("The value for device.rerouting.adaptation-weight must be between 0 and 1!");
            }
            if (myAdaptationWeight < 1. && myAdaptationInterval > 0) {
                myEdgeWeightSettingCommand = new StaticCommand<MSDevice_Routing>(&MSDevice_Routing::adaptEdgeEfforts);
                MSNet::getInstance()->getEndOfTimestepEvents().addEvent(
                    myEdgeWeightSettingCommand, 0, MSEventControl::ADAPT_AFTER_EXECUTION);
            } else if (period > 0 || prePeriod > 0) {
                WRITE_WARNING("Rerouting is useless if the edge weights do not get updated!");
            }
        }
        if (myWithTaz) {
            if (MSEdge::dictionary(v.getParameter().fromTaz + "-source") == 0) {
                WRITE_ERROR("Source district '" + v.getParameter().fromTaz + "' not known when rerouting '" + v.getID() + "'!");
                return;
            }
            if (MSEdge::dictionary(v.getParameter().toTaz + "-sink") == 0) {
                WRITE_ERROR("Destination district '" + v.getParameter().toTaz + "' not known when rerouting '" + v.getID() + "'!");
                return;
            }
        }
        // build the device
        into.push_back(new MSDevice_Routing(v, "routing_" + v.getID(), period, prePeriod));
    }
}


// ---------------------------------------------------------------------------
// MSDevice_Routing-methods
// ---------------------------------------------------------------------------
MSDevice_Routing::MSDevice_Routing(SUMOVehicle& holder, const std::string& id,
                                   SUMOTime period, SUMOTime preInsertionPeriod)
    : MSDevice(holder, id), myPeriod(period), myPreInsertionPeriod(preInsertionPeriod), myRerouteCommand(0) {
    // we do always a pre insertion reroute to fill the best lanes of the vehicle with somehow meaningful values (especially for deaprtLane="best")
    myRerouteCommand = new WrappingCommand<MSDevice_Routing>(this, &MSDevice_Routing::preInsertionReroute);
    // if we don't update the edge weights, we might as well reroute now and hopefully use our threads better
    const SUMOTime execTime = myEdgeWeightSettingCommand == 0 ? 0 : holder.getParameter().depart;
    MSNet::getInstance()->getInsertionEvents().addEvent(
        myRerouteCommand, execTime,
        MSEventControl::ADAPT_AFTER_EXECUTION);
}


MSDevice_Routing::~MSDevice_Routing() {
    // make the rerouting command invalid if there is one
    if (myRerouteCommand != 0) {
        myRerouteCommand->deschedule();
    }
}


bool
MSDevice_Routing::notifyEnter(SUMOVehicle& /*veh*/, MSMoveReminder::Notification reason) {
    if (reason == MSMoveReminder::NOTIFICATION_DEPARTED) {
        // clean up pre depart rerouting
        if (myPreInsertionPeriod > 0) {
            myRerouteCommand->deschedule();
        }
        myRerouteCommand = 0;
        // trigger on depart rerouting if the edge weights did change
        const SUMOTime now = MSNet::getInstance()->getCurrentTimeStep();
        if (myHolder.getParameter().depart <= myLastAdaptation && now > myLastAdaptation) {
            wrappedRerouteCommandExecute(now);
        }
        // build repetition trigger if routing shall be done more often
        if (myPeriod > 0) {
            myRerouteCommand = new WrappingCommand<MSDevice_Routing>(this, &MSDevice_Routing::wrappedRerouteCommandExecute);
            MSNet::getInstance()->getBeginOfTimestepEvents().addEvent(
                myRerouteCommand, myPeriod + now,
                MSEventControl::ADAPT_AFTER_EXECUTION);
        }
    }
    return false;
}


SUMOTime
MSDevice_Routing::preInsertionReroute(SUMOTime currentTime) {
    if (myWithTaz) {
        const MSEdge* source = MSEdge::dictionary(myHolder.getParameter().fromTaz + "-source");
        const MSEdge* dest = MSEdge::dictionary(myHolder.getParameter().toTaz + "-sink");
        if (source && dest) {
            const std::pair<const MSEdge*, const MSEdge*> key = std::make_pair(source, dest);
            if (myCachedRoutes.find(key) == myCachedRoutes.end()) {
                reroute(myHolder, currentTime, true);
            } else {
                myHolder.replaceRoute(myCachedRoutes[key], true);
            }
        }
    } else {
        reroute(myHolder, currentTime, true);
    }
    return myPreInsertionPeriod;
}


SUMOTime
MSDevice_Routing::wrappedRerouteCommandExecute(SUMOTime currentTime) {
    reroute(myHolder, currentTime);
    return myPeriod;
}


SUMOReal
MSDevice_Routing::getEffort(const MSEdge* const e, const SUMOVehicle* const v, SUMOReal) {
    const int id = e->getNumericalID();
    if (id < (int)myEdgeEfforts.size()) {
        return MAX2(myEdgeEfforts[id], e->getMinimumTravelTime(v));
    }
    return 0;
}


SUMOTime
MSDevice_Routing::adaptEdgeEfforts(SUMOTime currentTime) {
    std::map<std::pair<const MSEdge*, const MSEdge*>, const MSRoute*>::iterator it = myCachedRoutes.begin();
    for (; it != myCachedRoutes.end(); ++it) {
        it->second->release();
    }
    myCachedRoutes.clear();
    const SUMOReal newWeightFactor = (SUMOReal)(1. - myAdaptationWeight);
    const std::vector<MSEdge*>& edges = MSNet::getInstance()->getEdgeControl().getEdges();
    for (std::vector<MSEdge*>::const_iterator i = edges.begin(); i != edges.end(); ++i) {
        const int id = (*i)->getNumericalID();
        myEdgeEfforts[id] = myEdgeEfforts[id] * myAdaptationWeight + (*i)->getCurrentTravelTime() * newWeightFactor;
    }
    myLastAdaptation = currentTime;
    return myAdaptationInterval;
}


void
MSDevice_Routing::reroute(SUMOVehicle& v, const SUMOTime currentTime, const bool onInit) {
#ifdef HAVE_FOX
    const bool needThread = (myRouter == 0 && myThreadPool.getPending() + 1 > myThreadPool.size());
#endif
    if (myRouter == 0) {
        const std::string routingAlgorithm = OptionsCont::getOptions().getString("routing-algorithm");
        const bool mayHaveRestrictions = MSNet::getInstance()->hasRestrictions() || OptionsCont::getOptions().getInt("remote-port") != 0;
        if (routingAlgorithm == "dijkstra") {
            if (mayHaveRestrictions) {
                myRouter = new DijkstraRouterTT<MSEdge, SUMOVehicle, prohibited_withRestrictions<MSEdge, SUMOVehicle> >(
                    MSEdge::numericalDictSize(), true, &MSDevice_Routing::getEffort);
            } else {
                myRouter = new DijkstraRouterTT<MSEdge, SUMOVehicle, prohibited_noRestrictions<MSEdge, SUMOVehicle> >(
                    MSEdge::numericalDictSize(), true, &MSDevice_Routing::getEffort);
            }
        } else if (routingAlgorithm == "astar") {
            if (mayHaveRestrictions) {
                myRouter = new AStarRouter<MSEdge, SUMOVehicle, prohibited_withRestrictions<MSEdge, SUMOVehicle> >(
                    MSEdge::numericalDictSize(), true, &MSDevice_Routing::getEffort);
            } else {
                myRouter = new AStarRouter<MSEdge, SUMOVehicle, prohibited_noRestrictions<MSEdge, SUMOVehicle> >(
                    MSEdge::numericalDictSize(), true, &MSDevice_Routing::getEffort);
            }
#ifdef HAVE_INTERNAL // catchall for internal stuff
        } else if (routingAlgorithm == "bulkstar") {
            if (mayHaveRestrictions) {
                myRouter = new BulkStarRouterTT<MSEdge, SUMOVehicle, prohibited_withRestrictions<MSEdge, SUMOVehicle> >(
                    MSEdge::numericalDictSize(), true, &MSDevice_Routing::getEffort);
            } else {
                myRouter = new BulkStarRouterTT<MSEdge, SUMOVehicle, prohibited_noRestrictions<MSEdge, SUMOVehicle> >(
                    MSEdge::numericalDictSize(), true, &MSDevice_Routing::getEffort);
            }
        } else if (routingAlgorithm == "CH") {
            // defaultVehicle is only in constructor and may be safely deleted
            // it is mainly needed for its maximum speed. @todo XXX make this configurable
            ROVehicle defaultVehicle(SUMOVehicleParameter(), 0, net.getVehicleTypeSecure(DEFAULT_VTYPE_ID), &net);
            const SUMOTime begin = string2time(oc.getString("begin"));
            const SUMOTime weightPeriod = (oc.isSet("weight-files") ?
                                           string2time(oc.getString("weight-period")) :
                                           std::numeric_limits<int>::max());
            if (net.hasRestrictions()) {
                router = new CHRouter<ROEdge, ROVehicle, prohibited_withRestrictions<ROEdge, ROVehicle> >(
                    net.getEdgeNo(), oc.getBool("ignore-errors"), &ROEdge::getTravelTime, &defaultVehicle, begin, weightPeriod, true);
            } else {
                router = new CHRouter<ROEdge, ROVehicle, prohibited_noRestrictions<ROEdge, ROVehicle> >(
                    net.getEdgeNo(), oc.getBool("ignore-errors"), &ROEdge::getTravelTime, &defaultVehicle, begin, weightPeriod, false);
            }

        } else if (routingAlgorithm == "CHWrapper") {
            const SUMOTime begin = string2time(oc.getString("begin"));
            const SUMOTime weightPeriod = (oc.isSet("weight-files") ?
                                           string2time(oc.getString("weight-period")) :
                                           std::numeric_limits<int>::max());

            router = new CHRouterWrapper<ROEdge, ROVehicle, prohibited_withRestrictions<ROEdge, ROVehicle> >(
                net.getEdgeNo(), oc.getBool("ignore-errors"), &ROEdge::getTravelTime, begin, weightPeriod);

#endif // have HAVE_INTERNAL
        } else {
            throw ProcessError("Unknown routing algorithm '" + routingAlgorithm + "'!");
        }
    }
#ifdef HAVE_FOX
    if (needThread) {
        const int numThreads = OptionsCont::getOptions().getInt("device.rerouting.threads");
        if (myThreadPool.size() < numThreads) {
            new WorkerThread(myThreadPool, myRouter);
        }
        if (myThreadPool.size() < numThreads) {
            myRouter = 0;
        }
    }
    if (myThreadPool.size() > 0) {
        myThreadPool.add(new RoutingTask(v, currentTime, onInit));
        return;
    }
#endif
    v.reroute(currentTime, *myRouter, onInit);
}


void
MSDevice_Routing::cleanup() {
    delete myRouter;
    myRouter = 0;
}


#ifdef HAVE_FOX
void
MSDevice_Routing::waitForAll() {
    if (myThreadPool.size() > 0) {
        myThreadPool.waitAllAndClear();
    }
}


// ---------------------------------------------------------------------------
// MSDevice_Routing::RoutingTask-methods
// ---------------------------------------------------------------------------
void
MSDevice_Routing::RoutingTask::run(FXWorkerThread* context) {
    myVehicle.reroute(myTime, *((WorkerThread*)context)->myRouter, myOnInit);
    if (myOnInit) {
        const MSEdge* source = MSEdge::dictionary(myVehicle.getParameter().fromTaz + "-source");
        const MSEdge* dest = MSEdge::dictionary(myVehicle.getParameter().toTaz + "-sink");
        const std::pair<const MSEdge*, const MSEdge*> key = std::make_pair(source, dest);
        context->poolLock();
        if (MSDevice_Routing::myCachedRoutes.find(key) == MSDevice_Routing::myCachedRoutes.end()) {
            MSDevice_Routing::myCachedRoutes[key] = &myVehicle.getRoute();
            myVehicle.getRoute().addReference();
        }
        context->poolUnlock();
    }
}
#endif

        
/****************************************************************************/

