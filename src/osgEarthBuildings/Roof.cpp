
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2016 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "Roof"
#include "Elevation"
#include <osgEarthSymbology/Geometry>

using namespace osgEarth;
using namespace osgEarth::Symbology;
using namespace osgEarth::Buildings;

Roof::Roof() :
_parent     ( 0L ),
_hasModelBox( false )
{
    //nop
}

Config
Roof::getConfig() const
{
    Config conf("roof");
    return conf;
}

bool
Roof::build(const Footprint* footprint)
{
    if ( getModelSymbol() )
    {
        // calculate a 4-point boundary suitable for placing rooftop models.
        _hasModelBox = findRectangle( footprint, _modelBox );
    }
    return true;
}

namespace
{
    struct Line
    {
        osg::Vec3d _a, _b;

        Line(const osg::Vec3d& p0, const osg::Vec3d& p1) : _a(p0), _b(p1) { }

        // intersect this (as a line) with a line segment.
        bool intersectLineSeg(const Line& rhs, osg::Vec3d& out) const {
            double p0_x = _a.x(), p0_y = _a.y(), p1_x = _b.x(), p1_y = _b.y();
            double p2_x = rhs._a.x(), p2_y = rhs._a.y(), p3_x = rhs._b.x(), p3_y = rhs._b.y();
            
            double s1_x, s1_y, s2_x, s2_y;
            s1_x = p1_x - p0_x;     s1_y = p1_y - p0_y; 
            s2_x = p3_x - p2_x;     s2_y = p3_y - p2_y;
            
            double s, t, d;
            d = -s2_x * s1_y + s1_x * s2_y;
            if ( osg::equivalent(d, 0) )
                return false;

            s = (-s1_y * (p0_x - p2_x) + s1_x * (p0_y - p2_y)) / d;
            t = ( s2_x * (p0_y - p2_y) - s2_y * (p0_x - p2_x)) / d;

            if ( s >= 0 && s <= 1 ) //&& t >= 0 && t <= 1)
            {
                out.x() = p0_x + (t * s1_x);
                out.y() = p0_y + (t * s1_y);
                return true;
            }

            return false;
        }

        // intersect this (as a segment) with another segment.
        bool intersectSegSeg(const Line& rhs, osg::Vec3d& out) const {
            double p0_x = _a.x(), p0_y = _a.y(), p1_x = _b.x(), p1_y = _b.y();
            double p2_x = rhs._a.x(), p2_y = rhs._a.y(), p3_x = rhs._b.x(), p3_y = rhs._b.y();
            
            double s1_x, s1_y, s2_x, s2_y;
            s1_x = p1_x - p0_x;     s1_y = p1_y - p0_y; 
            s2_x = p3_x - p2_x;     s2_y = p3_y - p2_y;
            
            double s, t, d;
            d = -s2_x * s1_y + s1_x * s2_y;
            if ( osg::equivalent(d, 0) )
                return false;

            s = (-s1_y * (p0_x - p2_x) + s1_x * (p0_y - p2_y)) / d;
            t = ( s2_x * (p0_y - p2_y) - s2_y * (p0_x - p2_x)) / d;

            if ( s >= 0 && s <= 1 && t >= 0 && t <= 1)
            {
                out.x() = p0_x + (t * s1_x);
                out.y() = p0_y + (t * s1_y);
                return true;
            }

            return false;
        }
    };

    // True is the polygon/ring fully contains the 4-corner box.
    bool polyContainsBox(const Ring* poly, const osg::Vec3d* box)
    {
        for(int i=0; i<4; ++i) 
        {
            if ( !poly->contains2D(box[i].x(), box[i].y()) )
                return false;
        }
        
        Line a(box[0], box[1]), b(box[1], box[2]), c(box[2], box[3]), d(box[3], box[1]);
        
        osg::Vec3d unused;
        ConstSegmentIterator si(poly, true);
        while(si.hasMore())
        {
            Segment seg = si.next();
            Line s(seg.first, seg.second);
            if ( s.intersectSegSeg(a, unused) ) return false;
            if ( s.intersectSegSeg(b, unused) ) return false;
            if ( s.intersectSegSeg(c, unused) ) return false;
            if ( s.intersectSegSeg(d, unused) ) return false;
        }

        return true;
    }
}


bool
Roof::findRectangle(const Footprint* fp, osg::Vec3d* output) const
{
    // This algorithm attempts to find a suitable location and size
    // for a rectangle that lies inside the footprint. Later we can
    // extend it to try multiple different points and find the one
    // that yields the best result. Random scattering is one option,
    // a 3x3 grid is another option. For now it calculates a "centriod"
    // derived from the "longest edge" of the footprint (the same one
    // used to determine the buidling's rotation).

    // Inspiration:
    // http://d3plus.org/blog/behind-the-scenes/2014/07/08/largest-rect/

    // Find candidate center point. We start with the midpoint of the 
    // longest edge, which is calculated earlier in Elevation::setRotation.
    osg::Vec3d m = getParent()->getLongEdgeRotatedMidpoint();
    osg::Vec3d n = getParent()->getLongEdgeRotatedInsideNormal();

    // Two vectors (orthogonal) will form the "X" and "Y" axes
    // of our rotated rectangle.
    osg::Vec3d yvec = n;
    osg::Vec3d xvec = n^osg::Vec3d(0,0,1); xvec.normalize();

    // TEST:
    //m = fp->getBounds().center();


    // Extend a line out from the starting point along our "Y" axis
    // and intersect it with the far edge of the polygon. Then find the midpoint.
    Line yline(m, m+yvec);
    ConstSegmentIterator siY( fp, true );
    osg::Vec3d y0, y1;
    int num = 0;
    while(siY.hasMore() && num < 2)
    {
        Segment s = siY.next();
        Line seg(s.first, s.second);
        if ( num == 0 ) {
            if ( yline.intersectLineSeg(seg, y0) )
                ++num;
        }
        else {
            if ( yline.intersectLineSeg(seg, y1) )
                ++num;
        }
    }
    if ( num < 2 ) return false;

    osg::Vec3d p0 = (y0+y1)*0.5;

    // Next extend a line from this new point along our "X" axis,
    // finding where it intersects the outside of the polygon.
    // Again, find the midpoint of these two intersections.
    Line xline(p0, p0+xvec);
    ConstSegmentIterator siX( fp, true );
    osg::Vec3d x0, x1;
    num = 0;
    while(siX.hasMore() && num < 2)
    {
        Segment s = siX.next();
        Line seg(s.first, s.second);
        if ( num == 0 ) {
            if ( xline.intersectLineSeg(seg, x0) )
                ++num;
        }
        else {
            if ( xline.intersectLineSeg(seg, x1) )
                ++num;
        }
    }
    if ( num < 2 ) return false;
    
    osg::Vec3d p1 = (x0+x1)*0.5;

    // We now have a reasonable center point for our rectangle.
    // Calculate the maximum width and height based on distance 
    // from the edges of the polygon.
    double w = std::min( (p1-x0).length(), (p1-x1).length() );
    double h = std::min( (p1-y0).length(), (p1-y1).length() );

    // make a box based on the max height and width, and scale it
    // down until it fits or until we give up.
    for(double scale = 0.8; scale >= 0.2; scale -= 0.1)
    {
        osg::Vec3d dx = xvec*w*scale, dy = yvec*h*scale;
    
        output[0].set( p1-dx-dy );  
        output[1].set( p1+dx-dy );  
        output[2].set( p1+dx+dy );
        output[3].set( p1-dx+dy );

        if ( polyContainsBox(fp, output) )
        {
            return true;
        }
    }

    return false;
}
