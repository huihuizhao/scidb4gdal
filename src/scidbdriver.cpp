/*
Copyright (c) 2015 Marius Appel <marius.appel@uni-muenster.de>

This file is part of scidb4gdal. scidb4gdal is licensed under the MIT license.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
-----------------------------------------------------------------------------*/


// TODO:
// * Add different modes for handling chunks as different images
// * Add query params like trimming, projecting, ..
// * Add minimum chunk size (e.g. 1 MB), min(1MB, image size)
// * Test writing for image size < chunk size
// * Add chunksize and overlap parameter in connection string for array creation
// * Insert to existing array
// * (!!!) Define and implement test cases!
// * (!!!) Improved error handling, e.g. for writing to already existing arrays or including removal of temporary (load) arrays
// * (!!!) Progressor for reading and writing large files


#include "scidbdriver.h"
#include "shimclient.h"

#include "utils.h"
#include <iomanip>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

CPL_C_START
void GDALRegister_SciDB ( void );
CPL_C_END

/**
 * GDAL driver registration function
 */
void GDALRegister_SciDB()
{
    GDALDriver  *poDriver;

    if ( GDALGetDriverByName ( "SciDB" ) == NULL ) {
        poDriver = new GDALDriver();
        poDriver->SetDescription ( "SciDB" );
        //poDriver->SetMetadataItem ( GDAL_DCAP_RASTER, "YES");
        poDriver->SetMetadataItem ( GDAL_DMD_LONGNAME, "SciDB array driver" );
        poDriver->SetMetadataItem ( GDAL_DMD_HELPTOPIC, "frmt_scidb.html" );
        poDriver->pfnOpen = scidb4gdal::SciDBDataset::Open;
        poDriver->pfnIdentify = scidb4gdal::SciDBDataset::Identify;
        poDriver->pfnCreate = scidb4gdal::SciDBDataset::Create;
        poDriver->pfnDelete = scidb4gdal::SciDBDataset::Delete;
        GetGDALDriverManager()->RegisterDriver ( poDriver );
    }
}





namespace scidb4gdal
{

    struct ConnectionPars {

        string arrayname;
        string host;
        int port;
        string user;
        string passwd;

        ConnectionPars() : arrayname ( "" ), host ( "http://localhost" ), port ( 8080 ), user ( "scidb" ), passwd ( "scidb" ) {}

        static ConnectionPars *parseConnectionString ( const string &connstr ) {
            if ( connstr.substr ( 0, 6 ).compare ( "SCIDB:" ) != 0 ) {
                Utils::error ( "This is not a scidb4gdal connection string" );
            }

            ConnectionPars *out = new ConnectionPars();

            string astr = connstr.substr ( 6, connstr.length() - 6 ); // Remove SCIDB: from connection string
            vector<string> parts;
            boost::split ( parts, astr, boost::is_any_of ( ",; " ) ); // Split at whitespace, comma, semicolon
            for ( vector<string>::iterator it = parts.begin(); it != parts.end(); ++it ) {
                vector<string> kv;
                boost::split ( kv, *it, boost::is_any_of ( "=" ) ); // No colon because uf URL
                if ( kv.size() != 2 ) {
                    continue;
                }
                else {
                    if ( kv[0].compare ( "host" ) == 0 ) out->host  = ( kv[1] );
                    else if ( kv[0].compare ( "port" ) == 0 ) out->port = boost::lexical_cast<int> ( kv[1] );
                    else if ( kv[0].compare ( "array" ) == 0 ) out->arrayname = ( kv[1] );
                    else if ( kv[0].compare ( "user" ) == 0 ) out->user = ( kv[1] );
                    else if ( kv[0].compare ( "password" ) == 0 ) out->passwd = ( kv[1] );
                    else {
                        continue;
                    }
                }
            }
            return out;

        }


    };




    SciDBRasterBand::SciDBRasterBand ( SciDBDataset *poDS, SciDBSpatialArray *array, int nBand )
    {
        this->poDS = poDS;
        this->nBand = nBand;
        this->_array = array;

        eDataType = Utils::scidbTypeIdToGDALType ( _array->attrs[nBand].typeId ); // Data type is mapped from SciDB's attribute data type


        /* GDAL interprets x dimension as image rows and y dimension as image cols whereas our
         * implementation of spatial reference systems assumes x being easting and y being northing.
         * This makes the following code pretty messy in mixing x and y. */
        uint32_t nImgYSize ( 1 + _array->getXDim().high - _array->getXDim().low );
        uint32_t nImgXSize ( 1 + _array->getYDim().high - _array->getYDim().low );
        nBlockYSize = ( _array->getXDim().chunksize < nImgYSize ) ? _array->getXDim().chunksize : nImgYSize;
        nBlockXSize = ( _array->getYDim().chunksize < nImgXSize ) ? _array->getYDim().chunksize : nImgXSize;
    }


    SciDBRasterBand::~SciDBRasterBand()
    {
        FlushCache();
    }



    CPLErr SciDBRasterBand::GetStatistics ( int bApproxOK, int bForce, double *pdfMin, double *pdfMax, double *pdfMean, double *pdfStdDev )
    {

        SciDBAttributeStats stats = ( ( SciDBDataset * ) poDS )->getClient()->getAttributeStats ( *_array, this->nBand - 1 );

        *pdfMin = stats.min;
        *pdfMax = stats.max;
        *pdfMean = stats.mean;
        *pdfStdDev = stats.stdev;

        return CE_None;
    }


    CPLErr SciDBRasterBand::IReadBlock ( int nBlockXOff, int nBlockYOff, void *pImage )
    {
        //TODO: Improve error handling
        SciDBDataset *poGDS = ( SciDBDataset * ) poDS;

        /* GDAL interprets x dimension as image rows and y dimension as image cols whereas our
         * implementation of spatial reference systems assumes x being easting and y being northing.
         * This makes the following code pretty messy in mixing x and y. */
        int xmin = nBlockYOff * this->nBlockYSize + _array->getXDim().low;
        int xmax = xmin + this->nBlockYSize - 1;
        if ( xmax > _array->getXDim().high ) xmax = _array->getXDim().high;

        int ymin = nBlockXOff * this->nBlockXSize + _array->getYDim().low;
        int ymax = ymin + this->nBlockXSize - 1;
        if ( ymax > _array->getYDim().high ) ymax = _array->getYDim().high;


        // Read  and fetch data

        // Last blocks must be treated separately if covering area outside actual array: (0 | 1 | 2 | 3 || 4 | 5 | 6 | 7) vs. (0 | 1 | - | - || 3 | 4 | - | -) for 4x2 block with only 2x2 data
        // TODO: Check whether this works also for nBlockXSize != nBlockYSize
        if ( ( nBlockXOff + 1 ) *this->nBlockXSize > poGDS->nRasterXSize ) {
            // This is a bit of a hack...
            size_t dataSize = ( 1 + xmax - xmin ) * ( 1 + ymax - ymin ) * Utils::scidbTypeIdBytes ( _array->attrs[nBand - 1].typeId ); // This is smaller than the block size!
            void *buf = malloc ( dataSize );

            // Write to temporary buffer first
            poGDS->getClient()->getData ( *_array, nBand - 1, buf, xmin, ymin, xmax, ymax ); // GDAL bands start with 1, scidb attribute indexes with 0
            for ( uint32_t i = 0; i < ( 1 + xmax - xmin ); ++i ) {
                uint8_t *src  = & ( ( uint8_t * ) buf ) [i * ( 1 + ymax - ymin ) * Utils::scidbTypeIdBytes ( _array->attrs[nBand - 1].typeId )];
                // TODO: In the following call, check whether this->nBlockXSize must be replaced with this->nBlockYSize for unequally sized chunk dimensions
                uint8_t *dest = & ( ( uint8_t * ) pImage ) [i * this->nBlockXSize * Utils::scidbTypeIdBytes ( _array->attrs[nBand - 1].typeId )];
                memcpy ( dest, src, ( 1 + ymax - ymin ) *Utils::scidbTypeIdBytes ( _array->attrs[nBand - 1].typeId ) );
            }
            free ( buf );
        }
        else poGDS->getClient()->getData ( *_array, nBand - 1, pImage, xmin, ymin, xmax, ymax ); // GDAL bands start with 1, scidb attribute indexes with 0

        return CE_None;
    }




    CPLErr SciDBRasterBand::IWriteBlock ( int nBlockXOff, int nBlockYOff, void *pImage )
    {
        //TODO: Improve error handling

        SciDBDataset *poGDS = ( SciDBDataset * ) poDS;


        // 1. Compute array bounds from block offsets
        int xmin = nBlockYOff * this->nBlockYSize + _array->getXDim().low;
        int xmax = xmin + this->nBlockYSize - 1;
        if ( xmax > _array->getXDim().high ) xmax = _array->getXDim().high;

        int ymin = nBlockXOff * this->nBlockXSize + _array->getYDim().low;
        int ymax = ymin + this->nBlockXSize - 1;
        if ( ymax > _array->getYDim().high ) ymax = _array->getYDim().high;



        // Last blocks must be treated separately if covering area outside actual array: (0 | 1 | 2 | 3 || 4 | 5 | 6 | 7) vs. (0 | 1 | - | - || 3 | 4 | - | -) for 4x2 block with only 2x2 data
        bool xOuter = ( nBlockXOff + 1 ) * this->nBlockXSize > poGDS->nRasterXSize; // or >= ???
        if ( xOuter ) {
            // This is a bit of a hack...
            size_t pixelSize = 0;
            uint32_t nx = ( 1 + xmax - xmin );
            uint32_t ny = ( 1 + ymax - ymin );
            for ( uint16_t i = 0; i < _array->attrs.size(); ++i ) pixelSize += Utils::scidbTypeIdBytes ( _array->attrs[i].typeId );
            size_t dataSize = pixelSize *  nx * ny;
            void *buf = malloc ( dataSize );

            stringstream v;
            v << "Write to bounds: x=(" << xmin << "," << xmax << ") y=(" << ymin << "," << ymax << ") nx=" << nx << " ny=" << ny << " BLOCKSIZE X " << this->nBlockXSize << " BLOCKSIZE Y " << this->nBlockYSize;
            Utils::debug ( v.str() );

            // Write to temporary buffer first
            for ( uint32_t i = 0; i < nx; ++i ) {
                // TODO: In the following call, check whether this->nBlockXSize must be replaced with this->nBlockYSize for unequally sized chunk dimensions
                uint8_t *src  = & ( ( uint8_t * ) pImage ) [i * this->nBlockXSize * pixelSize];
                uint8_t *dest  = & ( ( uint8_t * ) buf ) [i * ny * pixelSize];
                memcpy ( dest, src, ny * pixelSize );
            }
            // Write from buf instead of pImage
            poGDS->getClient()->insertData ( *_array, buf, xmin, ymin, xmax, ymax );

            free ( buf );
        }
        else poGDS->getClient()->insertData ( *_array, pImage, xmin, ymin, xmax, ymax );


        return CE_None;
    }




    SciDBDataset::SciDBDataset ( const string &connstr )
    {

        // 1. parse connection string and extract the following values



        ConnectionPars *pars = ConnectionPars::parseConnectionString ( connstr );
        Utils::debug ( "Connection pars: host:" + pars->host + ";user:" + pars->user + ";passwd:" + pars->passwd + ";array:" + pars->arrayname + ";port:" + boost::lexical_cast<string> ( pars->port ) );


        // 2. Check validity of parameters
        if ( pars->arrayname == "" ) Utils::error ( "No array specified, currently not supported" );

        // 3. Create shim client
        _client = new ShimClient ( pars->host, pars->port, pars->user, pars->passwd );

        // 4. Request array metadata
        _array = _client->getArrayDesc ( pars->arrayname );

        if ( _array.dims.size() != 2 ) {
            Utils::error ( "GDAL works with two-dimensional arrays only" );
        }



        /* GDAL interprets x dimension as image rows and y dimension as image cols whereas our
         * implementation of spatial reference systems assumes x being easting and y being northing.
         * This makes the following code pretty messy in mixing x and y. */
        this->nRasterYSize = 1 + _array.getXDim().high - _array.getXDim().low ;
        this->nRasterXSize = 1 + _array.getYDim().high - _array.getYDim().low ;

        stringstream sd;
        sd << nRasterXSize << "x" << nRasterYSize;
        Utils::debug ( sd.str() );


        // Create GDAL Bands
        for ( uint32_t i = 0; i < _array.attrs.size(); ++i )
            this->SetBand ( i + 1, new SciDBRasterBand ( this, &_array, i ) );


        this->SetDescription ( connstr.c_str() );



        // TODO: Overviews?
        //poDS->oOvManager.Initialize ( poDS, poOpenInfo->pszFilename );

        delete pars;

    }



    CPLErr SciDBDataset::SetProjection ( const char *wkt )
    {
        _array.srtext = wkt;
        OGRSpatialReference srs ( wkt );
        srs.AutoIdentifyEPSG();
        _array.auth_name = srs.GetAuthorityName ( NULL );
        _array.auth_srid = boost::lexical_cast<uint32_t> ( srs.GetAuthorityCode ( NULL ) );
        char *proj4;
        srs.exportToProj4 ( &proj4 );
        _array.proj4text.assign ( proj4 );
        CPLFree ( proj4 );

        _array.xdim = SCIDB4GDAL_DEFAULT_XDIMNAME;
        _array.ydim = SCIDB4GDAL_DEFAULT_XDIMNAME;


        // Only perform database update, if affine transformation has already been set
        if ( !_array.affineTransform.isIdentity() ) {
            _client->updateSRS ( _array );
        }

        return CE_None;
    }


    CPLErr SciDBDataset::SetGeoTransform ( double *padfTransform )
    {

        AffineTransform a ( padfTransform[0], padfTransform[3], padfTransform[1], padfTransform[5], padfTransform[2], padfTransform[4] );
        _array.affineTransform = a;


//  stringstream sdebug;
//  sdebug << std::setprecision(15) << "SetGeoTransform(" << padfTransform[0] <<","<< padfTransform[1]<<","<< padfTransform[2]<<","<< padfTransform[3]<<","<< padfTransform[4]<<","<< padfTransform[5] << ")";
//  Utils::debug(sdebug.str());
//  Utils::debug(a.toString());
        // Only perform database update, if srs has already been set
        if ( !_array.srtext.empty() ) {
            _client->updateSRS ( _array );

        }
        return CE_None;
    }



    GDALDataset *SciDBDataset::Create ( const char *pszFilename, int nXSize, int nYSize, int nBands, GDALDataType eType, char   **papszParmList )
    {

        ConnectionPars *pars = ConnectionPars::parseConnectionString ( pszFilename );

        // Create array metadata structure
        SciDBSpatialArray array;
        array.name = pars->arrayname;

        // Attributes
        vector<SciDBAttribute> attrs;
        for ( int i = 0; i < nBands; ++i ) {
            SciDBAttribute a;
            a.nullable = false; // TODO
            a.typeId = Utils::gdalTypeToSciDBTypeId ( eType ); // All attribtues must have the same data type
            stringstream aname;
            aname << "band" << i + 1;
            a.name = aname.str();
            attrs.push_back ( a );
        }
        array.attrs = attrs;



        // Dimensions

        vector<SciDBDimension> dims;

        SciDBDimension dimx;
        dimx.low = 0;
        dimx.high = nYSize - 1;
        dimx.name = SCIDB4GDAL_DEFAULT_XDIMNAME;
        dimx.chunksize = SCIDB4GDAL_DEFAULT_BLOCKSIZE_X;
        dimx.typeId = "int64";

        SciDBDimension dimy;
        dimy.low = 0;
        dimy.high = nXSize - 1;
        dimy.name = SCIDB4GDAL_DEFAULT_YDIMNAME;
        dimy.chunksize = SCIDB4GDAL_DEFAULT_BLOCKSIZE_Y;
        dimy.typeId = "int64";

        // TODO: Check order
        dims.push_back ( dimx );
        dims.push_back ( dimy );

        array.dims = dims;

        // Spatial reference is handled automatically by GDAL calling SetGeoTransform() and SetProjection()


        // Create shim client
        ShimClient *client = new ShimClient ( pars->host, pars->port, pars->user, pars->passwd );


        // Create array in SciDB
        client->createArray ( array );


        delete client;
        return ( GDALDataset * ) GDALOpen ( pszFilename, GA_Update );
    }





    SciDBDataset::~SciDBDataset()
    {
        FlushCache();
        delete _client;
    }




    CPLErr SciDBDataset::GetGeoTransform ( double *padfTransform )
    {
        padfTransform[0] = _array.affineTransform._x0;
        padfTransform[1] = _array.affineTransform._a11;
        padfTransform[2] = _array.affineTransform._a12;
        padfTransform[3] = _array.affineTransform._y0;
        padfTransform[4] = _array.affineTransform._a21;
        padfTransform[5] = _array.affineTransform._a22;
        return CE_None;
    }



    const char *SciDBDataset::GetProjectionRef()
    {
        return _array.srtext.c_str();
    }




    int SciDBDataset::Identify ( GDALOpenInfo *poOpenInfo )
    {
        if ( poOpenInfo->pszFilename == NULL || !EQUALN ( poOpenInfo->pszFilename, "SCIDB:", 6 ) ) {
            return FALSE;
        }
        return TRUE;
    }


    CPLErr SciDBDataset::Delete ( const char *pszName )
    {
        // TODO
        return CE_None;
    }


    GDALDataset *SciDBDataset::Open ( GDALOpenInfo *poOpenInfo )
    {

        string connstr = poOpenInfo->pszFilename;


        // Check whether dataset matches SciDB dataset
        if ( !Identify ( poOpenInfo ) )
            return NULL;


        // Do not allow update access
//         if (poOpenInfo->eAccess == GA_Update)
//         {
//             CPLError(CE_Failure, CPLE_NotSupported, "scidb4gdal currently does not support update access to existing arrays.\n");
//             return NULL;
//         }


        // Create the dataset
        SciDBDataset *poDS;
        poDS = new SciDBDataset ( poOpenInfo->pszFilename );
        return ( poDS );
    }



}