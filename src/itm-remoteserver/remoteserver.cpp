/****************************************************************************/
/// @file    remoteserver.cpp
/// @author  Thimor Bohn <bohn@itm.uni-luebeck.de>
/// @date    2007/03/13
/// @version $Id$
///
/// socket server user to control sumo by remote client
/****************************************************************************/
// SUMO, Simulation of Urban MObility; see http://sumo.sourceforge.net/
// copyright : (C) 2001-2007
//  by DLR (http://www.dlr.de/) and ZAIK (http://www.zaik.uni-koeln.de/AFS)
/****************************************************************************/
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation; either version 2 of the License, or
//   (at your option) any later version.
//
/****************************************************************************/
// RemoteServer.cpp
// @author: Thimor Bohn <bohn@itm.uni-luebeck.de
// ===========================================================================
// compiler pragmas
// ===========================================================================


// ===========================================================================
// included modules
// ===========================================================================
#include "remoteconstants.h"
#include "remoteserver.h"
#include "foreign/tcpip/socket.h"
#include "foreign/tcpip/storage.h"
#include "utils/common/SUMOTime.h"
#include "microsim/MSNet.h"
#include "microsim/MSVehicleControl.h"
#include "microsim/MSVehicle.h"
#include "utils/geom/Position2D.h"
#include "microsim/MSEdge.h"
#include "microsim/MSRouteHandler.h"
#include "microsim/MSRouteLoaderControl.h"
#include "microsim/MSRouteLoader.h"

#include "microsim/MSEdgeControl.h"
#include "microsim/MSLane.h"

#include <string>
#include <map>
#include <iostream>
#include <cstdlib>

#ifdef CHECK_MEMORY_LEAKS
#include <foreign/nvwa/debug_new.h>
#endif // CHECK_MEMORY_LEAKS


// ===========================================================================
// used namespaces
// ===========================================================================
using namespace std;
using namespace tcpip;


// ===========================================================================
// method definitions
// ===========================================================================
namespace itm
{
	/*****************************************************************************/

	RemoteServer::RemoteServer()
	{
		OptionsCont &oc = OptionsCont::getOptions();

		port_ = oc.getInt("remote-port");
		endTime_ = oc.getInt("end");
		penetration_ = oc.getFloat("penetration");
		routeFile_ = oc.getString("route-files");
		isMapChanged_ = true;
		numEquippedVehicles_ = 0;
		closeConnection_ = false;
		netBoundary_ = NULL;
	}

	/*****************************************************************************/

	RemoteServer::~RemoteServer()
	{
		if (netBoundary_ != NULL) delete netBoundary_;
	}

	/*****************************************************************************/

	void
		RemoteServer::run()
	{
		try
		{
			// Opens listening socket
			std::cout << "***Starting server on port " << port_  << "***" << std::endl;
			Socket socket(port_);
			socket.accept();

			// When got here, a client has connected
			// Simulation should run until
			// 1. end time reached or
			// 2. got CMD_CLOSE or
			// 3. Client closes socket connection

			while (!closeConnection_)
			{
				Storage storIn;
				Storage storOut;
				// Read a message
				try
				{
					socket.receiveExact(storIn);
				}
				catch ( ... )
				{
					std::cerr << "Error while receiving command via Mobility Interface" << std::endl;
					exit(1);
				}

				while (storIn.valid_pos() && !closeConnection_)
				{
					// dispatch each command
					if (! dispatchCommand(storIn, storOut)) closeConnection_ = true;
				}

				try
				{
					// send out all answers as one storage
					socket.sendExact(storOut);
				}
				catch ( ... )
				{
					std::cerr << "Error while sending command via Mobility Interface" << std::endl;
					exit(1);
				}
			}
		}
		catch (RemoteException e)
		{
			cerr << e.what() << endl;
		}
		catch (SocketException e)
		{
			cerr << e.what() << endl;
		}
	}

	/*****************************************************************************/

	bool
		RemoteServer::dispatchCommand(tcpip::Storage& requestMsg, tcpip::Storage& respMsg)
	{
		int commandStart = requestMsg.position();
		int commandLength = requestMsg.readUnsignedByte();

		int commandId = requestMsg.readUnsignedByte();
		
		// dispatch commands
		switch (commandId)
		{
		case CMD_SIMSTEP:
			commandSimulationStep(requestMsg,respMsg);
			break;
		case CMD_CLOSE:
			commandCloseConnection(requestMsg, respMsg);
			break;
		case CMD_SETMAXSPEED:
			commandSetMaximumSpeed(requestMsg, respMsg);
			break;
		default:
			writeStatusCmd(respMsg, commandId, RTYPE_NOTIMPLEMENTED, "Command not implemented in sumo");
			return false;
		}

		if (requestMsg.position() != commandStart + commandLength)
		{
			writeStatusCmd(respMsg, commandId, RTYPE_ERR, "Wrong position in requestMessage after dispatching command");
			return false;
		}
		return true;
	}

	/*****************************************************************************/

	void
		RemoteServer::commandSetMaximumSpeed(tcpip::Storage& requestMsg, tcpip::Storage& respMsg)
		throw (RemoteException)
	{

		int extId = requestMsg.readInt(); // external node id (equipped vehicle number)
		float maxspeed = requestMsg.readFloat();
		std::string intId;
		convertExt2IntId(extId, intId);
		MSVehicle *veh = MSNet::getInstance()->getVehicleControl().getVehicle( intId );
		
		if ( veh == NULL )
		{
			writeStatusCmd(respMsg, CMD_SETMAXSPEED, RTYPE_ERR, "Can not retrieve node with given ID " + extId);
			return;
		}

		if (maxspeed>=0)
		{
			veh->setIndividualMaxSpeed( maxspeed );
		} else {
			veh->unsetIndividualMaxSpeed();
		}

		// create a reply message
		writeStatusCmd(respMsg, CMD_SETMAXSPEED, RTYPE_OK, "");

		return;
	}

	/*****************************************************************************/

	void
		RemoteServer::commandSimulationStep(tcpip::Storage& requestMsg, tcpip::Storage& respMsg)
		throw (RemoteException)
	{
		// TargetTime
		SUMOTime targetTime = static_cast<SUMOTime>(requestMsg.readDouble());
		if (targetTime > endTime_)
		{
			targetTime = endTime_;
		}

		// Position representation
		int resType = requestMsg.readUnsignedByte();
		if (resType != POSITION_2D && resType != POSITION_ROADMAP)
		{
			writeStatusCmd(respMsg, CMD_SIMSTEP, RTYPE_ERR, "Error: unsupported return format requested.");
			return;
		}


		// do simulation step
		MSNet *net = MSNet::getInstance();
		SUMOTime currentTime = net->getCurrentTimeStep();
		if (targetTime - currentTime > 0)
		{
			net->simulate(currentTime, targetTime);
			isMapChanged_ = true;
		}
		currentTime = net->getCurrentTimeStep();

		// prepare output
		try
		{
			// map containing all active equipped vehicles. maps external id to MSVehicle*
			map<int, const MSVehicle*> activeEquippedVehicles;
			// get access to all vehicles in simulation
			MSVehicleControl &vehControl = net->getVehicleControl();
			// iterate over all vehicles in simulation
			for (map<string, MSVehicle*>::const_iterator iter = vehControl.loadedVehBegin(); iter != vehControl.loadedVehEnd(); ++iter)
			{
				// selected vehicle
				const string vehicleId   = (*iter).first;
				const MSVehicle *vehicle = (*iter).second;
				// insert into equippedVehicleId if not contained
				if (equippedVehicles_.find(vehicleId) == equippedVehicles_.end())
				{
					// determine if vehicle is equipped
					double rnd = double(rand())/RAND_MAX;
					if (rnd <= penetration_)
					{
						// vehicle is equipped
						equippedVehicles_[vehicleId] = numEquippedVehicles_++;
					}
					else
					{
						// vehicle is not equipped
						equippedVehicles_[vehicleId] = -1;
					}
				}
				if (equippedVehicles_[vehicleId] >= 0 && vehicle->getInTransit())
				{
					int extId = equippedVehicles_[vehicleId];
					activeEquippedVehicles[extId] = vehicle;
					// vehicle is equipped
				}
			}

			// Everything is fine
			writeStatusCmd(respMsg, CMD_SIMSTEP, RTYPE_OK, "");

			//out.writeChar( static_cast<unsigned char>(rtype) );
			//out.writeInt(numEquippedVehicles_);
			// iterate over all active equipped vehicles
			// and generate a Move Node command for each vehicle
			for (map<int, const MSVehicle*>::iterator iter = activeEquippedVehicles.begin(); iter != activeEquippedVehicles.end(); ++iter)
			{
				int extId = (*iter).first;
				const MSVehicle* vehicle = (*iter).second;

				// command length
				respMsg.writeUnsignedByte(23);
				// command type
				respMsg.writeUnsignedByte(CMD_MOVENODE);
				// node id
				respMsg.writeInt(extId);
				// end time
				respMsg.writeDouble(currentTime);

				if (resType == POSITION_2D)
				{
					// return type
					respMsg.writeUnsignedByte(POSITION_2D);

					Position2D pos = vehicle->getPosition();
					//xpos
					respMsg.writeFloat(pos.x() - getNetBoundary().xmin());
					// y pos
					respMsg.writeFloat(pos.y() - getNetBoundary().ymin());
				} else if (resType == POSITION_ROADMAP)
				{
					// return type
					respMsg.writeUnsignedByte(POSITION_ROADMAP);

					respMsg.writeString(vehicle->getEdge()->getID());
					respMsg.writeFloat(vehicle->getPositionOnLane());
				}

			}

		}
		catch (...)
		{
			writeStatusCmd(respMsg, CMD_SIMSTEP, RTYPE_ERR, "some error happen in command: simulation step. Sumo shuts down.");
			return;
		}

		return;
	}

	/*****************************************************************************/

	void 
		RemoteServer::commandCloseConnection(tcpip::Storage& requestMsg, tcpip::Storage& respMsg) throw(RemoteException)
	{
		// Close simulation
		closeConnection_ = true;

		// write answer
		writeStatusCmd(respMsg, CMD_CLOSE, RTYPE_OK, "Goodbye");
	}

	/*****************************************************************************/

	void
		RemoteServer::writeStatusCmd(tcpip::Storage& respMsg, int commandId, int status, std::string description)
	{
		if (status == RTYPE_ERR)
		{
			closeConnection_ = true;
			cerr << "Answered with error to command " << commandId 
					<< ": " << description << endl;
		} else if (status == RTYPE_NOTIMPLEMENTED) 
		{
			cerr << "Requested command not implemented (" << commandId 
					<< "): " << description << endl;
		}

		// command length
		respMsg.writeUnsignedByte(1 + 1 + 1 + 4 + static_cast<int>(description.length()));
		// command type
		respMsg.writeUnsignedByte(commandId);
		// status
		respMsg.writeUnsignedByte(status);
		// description
		respMsg.writeString(description);

		return;
	}

	/*****************************************************************************/

	void 
		RemoteServer::convertExt2IntId(int extId, std::string& intId)
	{
		if (isMapChanged_)
		{
			isMapChanged_ = false;
			ext2intId.clear();
			for (map<std::string, int>::const_iterator iter = equippedVehicles_.begin(); iter != equippedVehicles_.end(); ++iter)
			{
				if (iter->second > -1)
				{
					ext2intId[iter->second] = iter->first;
				}
			}
		}

		// Search for external-Id-int and return internal-Id-string
		map<int, std::string>::const_iterator it = ext2intId.find(extId);
		if ( it != ext2intId.end() ) intId = it->second;
		else intId = "";
	}

	/*****************************************************************************/

	const Boundary&
		RemoteServer::getNetBoundary()
	{
		// If already calculated, just return the boundary
		if (netBoundary_ != NULL) return *netBoundary_;

		// Otherwise calculate it first
		netBoundary_ = new Boundary();
/*
		{
			// use the junctions to compute the boundaries
			for (size_t index=0; index<myNet.myJunctionWrapper.size(); index++) {
				if (myNet.myJunctionWrapper[index]->getShape().size()>0) {
					ret.add(myNet.myJunctionWrapper[index]->getBoundary());
				} else {
					ret.add(myNet.myJunctionWrapper[index]->getJunction().getPosition());
				}
			}
		}
*/
		// Get all edges
		MSEdgeControl& edges = MSNet::getInstance()->getEdgeControl();
		
		// Get Boundary of Single ...
		for (MSEdgeControl::EdgeCont::const_iterator edgeIt = edges.getSingleLaneEdges().begin(); 
			edgeIt != edges.getSingleLaneEdges().end(); ++edgeIt)
		{
			for (MSEdge::LaneCont::const_iterator laneIt = (*edgeIt)->getLanes()->begin();
				laneIt != (*edgeIt)->getLanes()->end(); ++laneIt)
			{
				netBoundary_->add((*laneIt)->getShape().getBoxBoundary());
			}
		}

		// ... and MultiLaneEdges
		for (MSEdgeControl::EdgeCont::const_iterator edgeIt = edges.getMultiLaneEdges().begin(); 
			edgeIt != edges.getMultiLaneEdges().end(); ++edgeIt)
		{
			for (MSEdge::LaneCont::const_iterator laneIt = (*edgeIt)->getLanes()->begin();
				laneIt != (*edgeIt)->getLanes()->end(); ++laneIt)
			{
				netBoundary_->add((*laneIt)->getShape().getBoxBoundary());
			}
		}

		// make the boundary slightly larger
		netBoundary_->grow(0.1);

		return *netBoundary_;
	}

	/*****************************************************************************/
}

