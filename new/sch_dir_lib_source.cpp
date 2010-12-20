/*
 * This program source code file is part of KICAD, a free EDA CAD application.
 *
 * Copyright (C) 2010 SoftPLC Corporation, <dick@softplc.com>
 * Copyright (C) 2010 Kicad Developers, see change_log.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */



/*  Note: this LIB_SOURCE implementation relies on the posix specified opendir() and
    related functions rather than wx functions which might do the same thing.  This
    is because I did not want to become very dependent on wxWidgets at such a low
    level as this, in case someday this code needs to be used on kde or whatever.

    Mingw and unix, linux, & osx will all have these posix functions.
    MS Visual Studio may need the posix compatible opendir() functions brought in
        http://www.softagalleria.net/dirent.php
    wx has these but they are based on wxString which can be wchar_t based and wx should
    not be introduced at a level this low.

    Part files: have the general form partname.part[.revN...]
    Categories: are any subdirectories immediately below the sourceURI, one level only.
    Part names: [category/]partname[/revN...]
*/


#include <sch_dir_lib_source.h>
using namespace SCH;

#include <kicad_exceptions.h>

#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <ctype.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <vector>
using namespace std;



/**
 * Class DIR_WRAP
 * provides a destructor which is invoked if an exception is thrown.
 */
class DIR_WRAP
{
    DIR*    dir;

public:
    DIR_WRAP( DIR* aDir ) : dir( aDir ) {}

    ~DIR_WRAP()
    {
        if( dir )
            closedir( dir );
    }

    DIR* operator->()   { return dir; }
    DIR* operator*()    { return dir; }
    operator bool ()    { return dir!=0; }
};


/**
 * Class FILE_WRAP
 * provides a destructor which is invoked if an exception is thrown.
 */
class FILE_WRAP
{
    int     fh;

public:
    FILE_WRAP( int aFileHandle ) : fh( aFileHandle ) {}
    ~FILE_WRAP()
    {
        if( fh != -1 )
            close( fh );
    }

    operator int ()  { return fh; }
};


/**
 * Function strrstr
 * finds the last instance of needle in haystack, if any.
 */
static const char* strrstr( const char* haystack, const char* needle )
{
    const char* ret = 0;
    const char* next = haystack;

    // find last instance of haystack
    while( (next = strstr( next, needle )) != 0 )
    {
        ret = next;
        ++next;     // don't keep finding the same one.
    }

    return ret;
}


/**
 * Function endsWithRev
 * returns a pointer to the final string segment: "revN[N..]" or NULL if none.
 * @param start is the beginning of string segment to test, the partname or
 *  any middle portion of it.
 * @param tail is a pointer to the terminating nul, or one past inclusive end of
 *  segment, i.e. the string segment of interest is [start,tail)
 * @param separator is the separating byte, expected: '.' or '/', depending on context.
 */
static const char* endsWithRev( const char* start, const char* tail, char separator )
{
    bool    sawDigit = false;

    while( tail>start && isdigit(*--tail) )
    {
        sawDigit = true;
    }

    // if sawDigit, tail points to the 'v' here.

    if( sawDigit && tail-3 >= start )
    {
        tail -= 3;

        if( tail[0]==separator && tail[1]=='r' && tail[2]=='e' && tail[3]=='v' )
        {
            return tail+1;  // omit separator, return "revN[N..]"
        }
    }

    return 0;
}

// see struct BY_REV
bool BY_REV::operator() ( const STRING& s1, const STRING& s2 ) const
{
    // avoid instantiating new STRINGs, and thank goodness that c_str() is const.

    const char* rev1 = endsWithRev( s1.c_str(), s1.c_str()+s1.size(), '/' );
    const char* rev2 = endsWithRev( s2.c_str(), s2.c_str()+s2.size(), '/' );

    int rootLen1 =  rev1 ? rev1 - s1.c_str() : s1.size();
    int rootLen2 =  rev2 ? rev2 - s2.c_str() : s2.size();

    int r = memcmp( s1.c_str(), s2.c_str(), min( rootLen1, rootLen2 ) );

    if( r )
    {
        return r < 0;
    }

    if( rootLen1 != rootLen2 )
    {
        return rootLen1 < rootLen2;
    }

    // root strings match at this point, compare the revision number numerically,
    // and chose the higher numbered version as "less", according to std::set lingo.

    if( bool(rev1) != bool(rev2) )
    {
        return bool(rev1) < bool(rev2);
    }

    if( rev1 && rev2 )
    {
        int rnum1 = atoi( rev1+3 );
        int rnum2 = atoi( rev2+3 );

        return rnum1 > rnum2;
    }

    return false;   // strings are equal, and they don't have a rev
}


bool DIR_LIB_SOURCE::makePartName( STRING* aPartName, const char* aEntry,
                        const STRING& aCategory )
{
    const char* cp = strrstr( aEntry, ".part" );

    // if base name is not empty, contains ".part", && cp is not NULL
    if( cp > aEntry )
    {
        const char* limit = cp + strlen( cp );

        // If versioning, then must find a trailing "revN.." type of string.
        if( useVersioning )
        {
            const char* rev = endsWithRev( cp + sizeof(".part") - 1, limit, '.' );
            if( rev )
            {
                if( aCategory.size() )
                    *aPartName = aCategory + "/";
                else
                    aPartName->clear();

                aPartName->append( aEntry, cp - aEntry );
                aPartName->append( "/" );
                aPartName->append( rev );
                return true;
            }
        }

        // If using versioning, then all valid partnames must have a rev string,
        // so we don't even bother to try and load any other partfile down here.
        else
        {
            // if file extension is exactly ".part", and no rev
            if( cp==limit-5 )
            {
                if( aCategory.size() )
                    *aPartName = aCategory + "/";
                else
                    aPartName->clear();

                aPartName->append( aEntry, cp - aEntry );
                return true;
            }
        }
    }

    return false;
}


STRING DIR_LIB_SOURCE::makeFileName( const STRING& aPartName )
{
    // create a fileName for the sweet string, using a reversible
    // partname <-> fileName conversion protocol:

    STRING  fileName = sourceURI + "/";

    const char* rev = endsWithRev( aPartName.c_str(), aPartName.c_str()+aPartName.size(), '/' );

    if( rev )
    {
        int basePartLen = rev - aPartName.c_str() - 1;  // omit '/' separator
        fileName.append( aPartName, 0,  basePartLen );
        fileName += ".part.";    // add '.' separator before rev
        fileName += rev;
    }
    else
    {
        fileName += aPartName;
        fileName += ".part";
    }

    return fileName;
}


void DIR_LIB_SOURCE::readSExpression( STRING* aResult, const STRING& aFilename ) throw( IO_ERROR )
{
    FILE_WRAP   fw = open( aFilename.c_str(), O_RDONLY );

    if( fw == -1 )
    {
        STRING  msg = strerror( errno );
        msg += "; cannot open(O_RDONLY) file " + aFilename;
        throw( IO_ERROR( msg.c_str() ) );
    }

    struct stat     fs;

    fstat( fw, &fs );

    // sanity check on file size
    if( fs.st_size > (1*1024*1024) )
    {
        STRING msg = aFilename;
        msg += " seems too big.  ( > 1 mbyte )";
        throw IO_ERROR( msg.c_str() );
    }

    // reuse same readBuffer, which is not thread safe, but the API
    // is not advertising thread safe (yet, if ever).
    if( (int) fs.st_size > (int) readBuffer.size() )
        readBuffer.resize( fs.st_size + 1000 );

    int count = read( fw, &readBuffer[0], fs.st_size );
    if( count != (int) fs.st_size )
    {
        STRING  msg = strerror( errno );
        msg += "; cannot read file " + aFilename;
        throw( IO_ERROR( msg.c_str() ) );
    }

    // std::string chars are not guaranteed to be contiguous in
    // future implementations of C++, so this is why we did not read into
    // aResult directly.
    aResult->assign( &readBuffer[0], count );
}


void DIR_LIB_SOURCE::cache() throw( IO_ERROR )
{
    partnames.clear();
    categories.clear();

    cacheOneDir( "" );
}


DIR_LIB_SOURCE::DIR_LIB_SOURCE( const STRING& aDirectoryPath,
                                const STRING& aOptions ) throw( IO_ERROR ) :
    useVersioning( strstr( aOptions.c_str(), "useVersioning" ) )
{
    sourceURI     = aDirectoryPath;
    sourceType    = "dir";

    if( sourceURI.size() == 0 )
    {
        throw( IO_ERROR( "aDirectoryPath cannot be empty" ) );
    }

    // remove any trailing separator, so we can add it back later without ambiguity
    if( strchr( "/\\", sourceURI[sourceURI.size()-1] ) )
        sourceURI.erase( sourceURI.size()-1 );

    cache();
}


DIR_LIB_SOURCE::~DIR_LIB_SOURCE()
{
}


void DIR_LIB_SOURCE::GetCategoricalPartNames( STRINGS* aResults, const STRING& aCategory )
    throw( IO_ERROR )
{
    aResults->clear();

    if( aCategory.size() )
    {
        STRING  lower = aCategory + "/";
        STRING  upper = aCategory + char( '/' + 1 );

        PART_CACHE::const_iterator limit = partnames.upper_bound( upper );

        for( PART_CACHE::const_iterator it = partnames.lower_bound( lower );  it!=limit;  ++it )
        {
            /*
            const char* start = it->c_str();
            size_t      len   = it->size();

            if( endsWithRev( start, start+len, '/' ) )
                continue;
            */

            aResults->push_back( *it );
        }
    }
    else
    {
        for( PART_CACHE::const_iterator it = partnames.begin();  it!=partnames.end();  ++it )
        {
            /*
            const char* start = it->c_str();
            size_t      len   = it->size();

            if( !endsWithRev( start, start+len, '/' ) )
                continue;
            */

            aResults->push_back( *it );
        }
    }
}


void DIR_LIB_SOURCE::ReadPart( STRING* aResult, const STRING& aPartName, const STRING& aRev )
    throw( IO_ERROR )
{
    STRING  partname = aPartName;

    if( aRev.size() )
        partname += "/" + aRev;

    PART_CACHE::const_iterator it = partnames.find( partname );

    if( it == partnames.end() )    // part not found
    {
        partname += " not found.";
        throw IO_ERROR( partname.c_str() );
    }

    // create a fileName for the sweet string
    STRING  fileName = makeFileName( aPartName );

    // @todo what about aRev?, and define the public API wrt to aRev better.

    readSExpression( aResult, fileName );
}


void DIR_LIB_SOURCE::ReadParts( STRINGS* aResults, const STRINGS& aPartNames )
    throw( IO_ERROR )
{
    aResults->clear();

    for( STRINGS::const_iterator n = aPartNames.begin();  n!=aPartNames.end();  ++n )
    {
        aResults->push_back( STRING() );
        ReadPart( &aResults->back(), *n );
    }
}


void DIR_LIB_SOURCE::GetCategories( STRINGS* aResults ) throw( IO_ERROR )
{
    aResults->clear();

    // caller fetches them sorted.
    for( NAME_CACHE::const_iterator it = categories.begin();  it!=categories.end();  ++it )
    {
        aResults->push_back( *it );
    }
}


#if defined(DEBUG)

void DIR_LIB_SOURCE::Show()
{
    printf( "Show categories:\n" );
    for( NAME_CACHE::const_iterator it = categories.begin();  it!=categories.end();  ++it )
        printf( " '%s'\n", it->c_str() );

    printf( "\n" );
    printf( "Show parts:\n" );
    for( PART_CACHE::const_iterator it = partnames.begin();  it != partnames.end();  ++it )
    {
        printf( " '%s'\n", it->c_str() );
    }
}

#endif


void DIR_LIB_SOURCE::cacheOneDir( const STRING& aCategory ) throw( IO_ERROR )
{
    STRING      curDir = sourceURI;

    if( aCategory.size() )
        curDir += "/" + aCategory;

    DIR_WRAP    dir = opendir( curDir.c_str() );

    if( !dir )
    {
        STRING  msg = strerror( errno );
        msg += "; scanning directory " + curDir;
        throw( IO_ERROR( msg.c_str() ) );
    }

    struct stat     fs;
    STRING          partName;
    STRING          fileName;
    dirent*         entry;

    while( (entry = readdir( *dir )) != NULL )
    {
        if( !strcmp( ".", entry->d_name ) || !strcmp( "..", entry->d_name ) )
            continue;

        fileName = curDir + "/" + entry->d_name;

        if( !stat( fileName.c_str(), &fs ) )
        {
            // is this a valid part name?
            if( S_ISREG( fs.st_mode ) && makePartName( &partName, entry->d_name, aCategory ) )
            {
                std::pair<NAME_CACHE::iterator, bool> pair = partnames.insert( partName );

                if( !pair.second )
                {
                    STRING  msg = partName;
                    msg += " has already been encountered";
                    throw IO_ERROR( msg.c_str() );
                }
            }

            // is this an acceptable category name?
            else if( S_ISDIR( fs.st_mode ) && !aCategory.size() && isCategoryName( entry->d_name ) )
            {
                // only one level of recursion is used, controlled by the
                // emptiness of aCategory.
                categories.insert( entry->d_name );

                // somebody needs to test Windows (mingw), make sure it can
                // handle opendir() recursively
                cacheOneDir( entry->d_name );
            }
            else
            {
                //D( printf( "ignoring %s\n", entry->d_name );)
            }
        }
    }
}


#if (1 || defined( TEST_DIR_LIB_SOURCE )) && defined(DEBUG)

int main( int argc, char** argv )
{
    STRINGS     partnames;
    STRINGS     sweets;

    try
    {
//        DIR_LIB_SOURCE  uut( argv[1] ? argv[1] : "", "" );
        DIR_LIB_SOURCE  uut( argv[1] ? argv[1] : "", "useVersioning" );

        // initially, only the NAME_CACHE sweets and STRING categories are loaded:
        uut.Show();

        uut.GetCategoricalPartNames( &partnames, "Category" );

        printf( "\nGetCategoricalPartNames( aCatagory = 'Category' ):\n" );
        for( STRINGS::const_iterator it = partnames.begin();  it!=partnames.end();  ++it )
        {
            printf( " '%s'\n", it->c_str() );
        }

        uut.ReadParts( &sweets, partnames );

        // fetch the part names for ALL categories.
        uut.GetCategoricalPartNames( &partnames );

        printf( "\nGetCategoricalPartNames( aCategory = '' i.e. ALL):\n" );
        for( STRINGS::const_iterator it = partnames.begin();  it!=partnames.end();  ++it )
        {
            printf( " '%s'\n", it->c_str() );
        }

        uut.ReadParts( &sweets, partnames );

        printf( "\nSweets for ALL parts:\n" );
        STRINGS::const_iterator pn = partnames.begin();
        for( STRINGS::const_iterator it = sweets.begin();  it!=sweets.end();  ++it, ++pn )
        {
            printf( " %s: %s", pn->c_str(), it->c_str() );
        }
    }

    catch( std::exception& ex )
    {
        printf( "std::exception\n" );
    }

    catch( IO_ERROR ioe )
    {
        printf( "exception: %s\n", (const char*) wxConvertWX2MB( ioe.errorText ) );
    }

    return 0;
}

#endif

