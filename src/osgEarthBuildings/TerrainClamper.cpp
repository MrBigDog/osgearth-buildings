
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
#include "TerrainClamper"

using namespace osgEarth;
using namespace osgEarth::Features;
using namespace osgEarth::Buildings;

#define LC "[TerrainClamper] "

TerrainClamper::TerrainClamper()
{
    //nop
}

void TerrainClamper::setSession(Session* session)
{
    _frame = session->createMapFrame();
}

bool
TerrainClamper::fetchTileFromMap(const TileKey& key, Tile* tile)
{
    const int tileSize = 33;

    tile->_loadTime = osg::Timer::instance()->tick();

    osg::ref_ptr<osg::HeightField> hf = new osg::HeightField();
    hf->allocate( tileSize, tileSize );

    // Initialize the heightfield to nodata
    hf->getFloatArray()->assign( hf->getFloatArray()->size(), NO_DATA_VALUE );

    TileKey keyToUse = key;
    while( !tile->_hf.valid() && keyToUse.valid() )
    {
        if (_frame.populateHeightField(hf, keyToUse, false /*heightsAsHAE*/, 0L))
        {
            tile->_hf = GeoHeightField( hf.get(), keyToUse.getExtent() );
            tile->_bounds = keyToUse.getExtent().bounds();
        }
        else
        {
            keyToUse = keyToUse.createParentKey();
        }
    }

    return tile->_hf.valid();
}

bool
TerrainClamper::getTile(const TileKey& key, osg::ref_ptr<Tile>& out)
{
    // first see whether the tile is available
    _tilesMutex.lock();

    osg::ref_ptr<Tile>& tile = _tiles[key];
    if ( !tile.valid() )
    {
        tile = new Tile();
    }
       
    if ( tile->_status == STATUS_EMPTY )
    {
        //OE_INFO << "  getTile(" << key.str() << ") -> fetch from map\n";
        tile->_status.exchange(STATUS_IN_PROGRESS);
        _tilesMutex.unlock();

        bool ok = fetchTileFromMap(key, tile);
        tile->_status.exchange( ok ? STATUS_AVAILABLE : STATUS_FAIL );
        
        out = ok ? tile.get() : 0L;
        return ok;
    }

    else if ( tile->_status == STATUS_AVAILABLE )
    {
        //OE_INFO << "  getTile(" << key.str() << ") -> available\n";
        out = tile.get();
        _tilesMutex.unlock();
        return true;
    }

    else if ( tile->_status == STATUS_FAIL )
    {
        //OE_INFO << "  getTile(" << key.str() << ") -> fail\n";
        _tilesMutex.unlock();
        out = 0L;
        return false;
    }

    else //if ( tile->_status == STATUS_IN_PROGRESS )
    {
        //OE_INFO << "  getTile(" << key.str() << ") -> in progress...waiting\n";
        _tilesMutex.unlock();
        return true;            // out:NULL => check back later please.
    }
}

bool
TerrainClamper::buildQuerySet(const GeoExtent& extent, unsigned lod, QuerySet& output)
{
    output.clear();

    if ( _frame.needsSync() )
    {
        if (_frame.sync())
        {
            _tilesMutex.lock();
            _tiles.clear();
            _tilesMutex.unlock();
        }
    }

    std::vector<TileKey> keys;
    _frame.getProfile()->getIntersectingTiles(extent, lod, keys);
    
    for(int i=0; i<keys.size(); ++i)
    {
        OE_START_TIMER(get);

        const double timeout = 10.0;
        osg::ref_ptr<Tile> tile;
        while( getTile(keys[i], tile) && !tile.valid() && OE_GET_TIMER(get) < timeout)
        {
            OpenThreads::Thread::YieldCurrentThread();
        }

        if ( !tile.valid() && OE_GET_TIMER(get) >= timeout )
        {
            OE_WARN << LC << "Timout fetching tile " << keys[i].str() << std::endl;
        }

        if ( tile.valid() )
        {
            if ( tile->_hf.valid() )
            {
                output.push_back( tile.get() );
            }
            else
            {
                OE_WARN << LC << "Got a tile with an invalid HF (" << keys[i].str() << ")\n";
            }
        }
    }

    return true;
}

TerrainEnvelope*
TerrainClamper::createEnvelope(const GeoExtent& extent, unsigned lod)
{
    TerrainEnvelope* e = new TerrainEnvelope();
    buildQuerySet( extent, lod, e->_tiles );
    return e;
}

//...................................

bool
TerrainEnvelope::getElevationExtrema(const Feature* feature, float& min, float& max) const
{
    if ( !feature || !feature->getGeometry() )
        return false;

    min = FLT_MAX, max = -FLT_MAX;

    ConstGeometryIterator gi(feature->getGeometry(), false);
    while(gi.hasMore())
    {
        const Geometry* part = gi.next();

        for(Geometry::const_iterator v = part->begin(); v != part->end(); ++v)
        {
            // TODO: consider speed of this vs. Profile::createTileKey + std::map lookup
            for(TerrainClamper::QuerySet::const_iterator q = _tiles.begin(); q != _tiles.end(); ++q)
            {
                TerrainClamper::Tile* tile = q->get();
                if ( tile->_bounds.contains(v->x(), v->y()) )
                {
                    float elevation;
                    if ( tile->_hf.getElevation(0L, v->x(), v->y(), INTERP_BILINEAR, 0L, elevation) )
                    {
                        if ( elevation < min ) min = elevation;
                        if ( elevation > max ) max = elevation;
                    }
                    break;
                }
            }
        }
    }

    return (min < max);
}