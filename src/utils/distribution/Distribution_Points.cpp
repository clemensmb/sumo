/****************************************************************************/
/// @file    Distribution_Points.cpp
/// @author  Daniel Krajzewicz
/// @date    Sept 2002
/// @version $Id: $
///
// The description of a distribution by a curve
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
// ===========================================================================
// compiler pragmas
// ===========================================================================
#ifdef _MSC_VER
#pragma warning(disable: 4786)
#endif


// ===========================================================================
// included modules
// ===========================================================================
#ifdef WIN32
#include <windows_config.h>
#else
#include <config.h>
#endif

#include <cassert>
#include "Distribution.h"
#include <utils/geom/Position2DVector.h>
#include "Distribution_Points.h"
#include <utils/common/StdDefs.h>

#ifdef _DEBUG
#include <utils/dev/debug_new.h>
#endif // _DEBUG


// ===========================================================================
// method definitions
// ===========================================================================
Distribution_Points::Distribution_Points(const std::string &id,
        const Position2DVector &points,
        bool interpolating)
        : Distribution(id), myPoints(points), myProbabilitiesAreComputed(false),
        myInterpolateDist(interpolating)
{}


Distribution_Points::~Distribution_Points()
{}


SUMOReal
Distribution_Points::getMax() const
{
    assert(myPoints.size()>0);
    const Position2D &p = myPoints[-1];
    return p.x();
}


size_t
Distribution_Points::getAreaNo() const
{
    return myPoints.size()-1;
}


SUMOReal
Distribution_Points::getAreaBegin(size_t index) const
{
    return myPoints[index].x();
}


SUMOReal
Distribution_Points::getAreaEnd(size_t index) const
{
    return myPoints[index+1].x();
}


SUMOReal
Distribution_Points::getAreaPerc(size_t index) const
{
    if (!myProbabilitiesAreComputed) {
        SUMOReal sum = 0;
        size_t i;
        if (myInterpolateDist) {
            for (i=0; i<myPoints.size()-1; i++) {
                SUMOReal width = getAreaEnd(i) - getAreaBegin(i);
                SUMOReal minval = MIN2(myPoints[i].y(), myPoints[i].y());
                SUMOReal maxval = MAX2(myPoints[i].y(), myPoints[i].y());
                SUMOReal amount = minval * width + (maxval-minval) * width / (SUMOReal) 2.;
                myProbabilities.push_back(amount);
                sum += amount;
            }
        } else {
            for (i=0; i<myPoints.size()-1; i++) {
                myProbabilities.push_back(myPoints[i].y());
                sum += myPoints[i].y();
            }
        }
        // normalize
        if (myInterpolateDist) {
            for (i=0; i<myPoints.size()-1; i++) {
                myProbabilities[i] = myProbabilities[i] / sum;
            }
        } else {
            for (i=0; i<myPoints.size()-1; i++) {
                myProbabilities[i] = myProbabilities[i] / sum;
            }
        }
        myProbabilitiesAreComputed = true;
    }
    return myProbabilities[index];
}



/****************************************************************************/

