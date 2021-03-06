/*
 * This program source code file is part of KICAD, a free EDA CAD application.
 *
 * Copyright (C) 2012 Torsten Hueter, torstenhtr <at> gmx.de
 * Copyright (C) 2012 Kicad Developers, see change_log.txt for contributors.
 * Copyright (C) 2017 CERN
 * @author Maciej Suminski <maciej.suminski@cern.ch>
 *
 * CAIRO_GAL - Graphics Abstraction Layer for Cairo
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

#include <wx/image.h>
#include <wx/log.h>

#include <gal/cairo/cairo_gal.h>
#include <gal/cairo/cairo_compositor.h>
#include <gal/definitions.h>
#include <geometry/shape_poly_set.h>

#include <limits>

#include <pixman.h>

using namespace KIGFX;



CAIRO_GAL::CAIRO_GAL( GAL_DISPLAY_OPTIONS& aDisplayOptions,
        wxWindow* aParent, wxEvtHandler* aMouseListener,
        wxEvtHandler* aPaintListener, const wxString& aName ) :
    GAL( aDisplayOptions ),
    wxWindow( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxEXPAND, aName )
{
    parentWindow  = aParent;
    mouseListener = aMouseListener;
    paintListener = aPaintListener;

    // Initialize the flags
    isGrouping          = false;
    isInitialized       = false;
    validCompositor     = false;
    groupCounter        = 0;

    // Connecting the event handlers
    Connect( wxEVT_PAINT,       wxPaintEventHandler( CAIRO_GAL::onPaint ) );

    // Mouse events are skipped to the parent
    Connect( wxEVT_MOTION,          wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_LEFT_DOWN,       wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_LEFT_UP,         wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_LEFT_DCLICK,     wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_MIDDLE_DOWN,     wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_MIDDLE_UP,       wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_MIDDLE_DCLICK,   wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_RIGHT_DOWN,      wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_RIGHT_UP,        wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_RIGHT_DCLICK,    wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
    Connect( wxEVT_MOUSEWHEEL,      wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
#if defined _WIN32 || defined _WIN64
    Connect( wxEVT_ENTER_WINDOW,    wxMouseEventHandler( CAIRO_GAL::skipMouseEvent ) );
#endif

    SetSize( aParent->GetSize() );
    screenSize = VECTOR2I( aParent->GetSize() );

    // Grid color settings are different in Cairo and OpenGL
    SetGridColor( COLOR4D( 0.1, 0.1, 0.1, 0.8 ) );
    SetAxesColor( COLOR4D( BLUE ) );

    // Allocate memory for pixel storage
    allocateBitmaps();
}


CAIRO_GAL::~CAIRO_GAL()
{
    deinitSurface();
    deleteBitmaps();

    ClearCache();
}


bool CAIRO_GAL::updatedGalDisplayOptions( const GAL_DISPLAY_OPTIONS& aOptions )
{
    bool refresh = false;

    if( super::updatedGalDisplayOptions( aOptions ) )
    {
        Refresh();
        refresh = true;
    }

    return refresh;
}


void CAIRO_GAL::BeginDrawing()
{
    initSurface();

    if( !validCompositor )
        setCompositor();

    compositor->SetMainContext( context );
    compositor->SetBuffer( mainBuffer );
}


void CAIRO_GAL::EndDrawing()
{
    // Force remaining objects to be drawn
    Flush();

    // Merge buffers on the screen
    compositor->DrawBuffer( mainBuffer );
    compositor->DrawBuffer( overlayBuffer );

    // Now translate the raw context data from the format stored
    // by cairo into a format understood by wxImage.
    pixman_image_t* dstImg = pixman_image_create_bits(PIXMAN_r8g8b8,
            screenSize.x, screenSize.y, (uint32_t*)wxOutput, wxBufferWidth * 3 );
    pixman_image_t* srcImg = pixman_image_create_bits(PIXMAN_a8b8g8r8,
            screenSize.x, screenSize.y, (uint32_t*)bitmapBuffer, wxBufferWidth * 4 );

    pixman_image_composite (PIXMAN_OP_SRC, srcImg, NULL, dstImg,
            0, 0, 0, 0, 0, 0, screenSize.x, screenSize.y );

    // Free allocated memory
    pixman_image_unref( srcImg );
    pixman_image_unref( dstImg );

    wxImage img( wxBufferWidth, screenSize.y, (unsigned char*) wxOutput, true );
    wxBitmap bmp( img );
    wxMemoryDC mdc( bmp );
    wxClientDC clientDC( this );

    // Now it is the time to blit the mouse cursor
    blitCursor( mdc );
    clientDC.Blit( 0, 0, screenSize.x, screenSize.y, &mdc, 0, 0, wxCOPY );

    deinitSurface();
}


void CAIRO_GAL::DrawLine( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint )
{
    cairo_move_to( currentContext, aStartPoint.x, aStartPoint.y );
    cairo_line_to( currentContext, aEndPoint.x, aEndPoint.y );
    flushPath();
    isElementAdded = true;
}


void CAIRO_GAL::DrawSegment( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint,
                             double aWidth )
{
    if( isFillEnabled )
    {
        // Filled tracks mode
        SetLineWidth( aWidth );

        cairo_move_to( currentContext, (double) aStartPoint.x, (double) aStartPoint.y );
        cairo_line_to( currentContext, (double) aEndPoint.x, (double) aEndPoint.y );
        cairo_set_source_rgba( currentContext, fillColor.r, fillColor.g, fillColor.b, fillColor.a );
        cairo_stroke( currentContext );
    }
    else
    {
        // Outline mode for tracks
        VECTOR2D startEndVector = aEndPoint - aStartPoint;
        double   lineAngle      = atan2( startEndVector.y, startEndVector.x );
        double   lineLength     = startEndVector.EuclideanNorm();

        cairo_save( currentContext );

        cairo_set_source_rgba( currentContext, strokeColor.r, strokeColor.g, strokeColor.b, strokeColor.a );

        cairo_translate( currentContext, aStartPoint.x, aStartPoint.y );
        cairo_rotate( currentContext, lineAngle );

        cairo_arc( currentContext, 0.0,        0.0, aWidth / 2.0,  M_PI / 2.0, 3.0 * M_PI / 2.0 );
        cairo_arc( currentContext, lineLength, 0.0, aWidth / 2.0, -M_PI / 2.0, M_PI / 2.0 );

        cairo_move_to( currentContext, 0.0,        aWidth / 2.0 );
        cairo_line_to( currentContext, lineLength, aWidth / 2.0 );

        cairo_move_to( currentContext, 0.0,        -aWidth / 2.0 );
        cairo_line_to( currentContext, lineLength, -aWidth / 2.0 );

        cairo_restore( currentContext );
        flushPath();
    }

    isElementAdded = true;
}


void CAIRO_GAL::DrawCircle( const VECTOR2D& aCenterPoint, double aRadius )
{
    cairo_new_sub_path( currentContext );
    cairo_arc( currentContext, aCenterPoint.x, aCenterPoint.y, aRadius, 0.0, 2 * M_PI );
    flushPath();
    isElementAdded = true;
}


void CAIRO_GAL::DrawArc( const VECTOR2D& aCenterPoint, double aRadius, double aStartAngle,
                         double aEndAngle )
{
    SWAP( aStartAngle, >, aEndAngle );

    cairo_new_sub_path( currentContext );
    cairo_arc( currentContext, aCenterPoint.x, aCenterPoint.y, aRadius, aStartAngle, aEndAngle );

    if( isFillEnabled )
    {
        VECTOR2D startPoint( cos( aStartAngle ) * aRadius + aCenterPoint.x,
                             sin( aStartAngle ) * aRadius + aCenterPoint.y );
        VECTOR2D endPoint( cos( aEndAngle ) * aRadius + aCenterPoint.x,
                           sin( aEndAngle ) * aRadius + aCenterPoint.y );

        cairo_move_to( currentContext, aCenterPoint.x, aCenterPoint.y );
        cairo_line_to( currentContext, startPoint.x, startPoint.y );
        cairo_line_to( currentContext, endPoint.x, endPoint.y );
        cairo_close_path( currentContext );
    }

    flushPath();

    isElementAdded = true;
}


void CAIRO_GAL::DrawArcSegment( const VECTOR2D& aCenterPoint, double aRadius, double aStartAngle,
                                double aEndAngle, double aWidth )
{
    SWAP( aStartAngle, >, aEndAngle );

    if( isFillEnabled )
    {
        cairo_arc( currentContext, aCenterPoint.x, aCenterPoint.y, aRadius, aStartAngle, aEndAngle );
        cairo_set_source_rgba( currentContext, fillColor.r, fillColor.g, fillColor.b, fillColor.a );
        cairo_stroke( currentContext );
    }
    else
    {
        double width = aWidth / 2.0;
        VECTOR2D startPoint( cos( aStartAngle ) * aRadius,
                             sin( aStartAngle ) * aRadius );
        VECTOR2D endPoint( cos( aEndAngle ) * aRadius,
                           sin( aEndAngle ) * aRadius );

        cairo_save( currentContext );

        cairo_set_source_rgba( currentContext, strokeColor.r, strokeColor.g, strokeColor.b, strokeColor.a );

        cairo_translate( currentContext, aCenterPoint.x, aCenterPoint.y );

        cairo_new_sub_path( currentContext );
        cairo_arc( currentContext, 0, 0, aRadius - width, aStartAngle, aEndAngle );

        cairo_new_sub_path( currentContext );
        cairo_arc( currentContext, 0, 0, aRadius + width, aStartAngle, aEndAngle );

        cairo_new_sub_path( currentContext );
        cairo_arc_negative( currentContext, startPoint.x, startPoint.y, width, aStartAngle, aStartAngle + M_PI );

        cairo_new_sub_path( currentContext );
        cairo_arc( currentContext, endPoint.x, endPoint.y, width, aEndAngle, aEndAngle + M_PI );

        cairo_restore( currentContext );
        flushPath();
    }

    isElementAdded = true;
}


void CAIRO_GAL::DrawRectangle( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint )
{
    // Calculate the diagonal points
    VECTOR2D diagonalPointA( aEndPoint.x,  aStartPoint.y );
    VECTOR2D diagonalPointB( aStartPoint.x, aEndPoint.y );

    // The path is composed from 4 segments
    cairo_move_to( currentContext, aStartPoint.x, aStartPoint.y );
    cairo_line_to( currentContext, diagonalPointA.x, diagonalPointA.y );
    cairo_line_to( currentContext, aEndPoint.x, aEndPoint.y );
    cairo_line_to( currentContext, diagonalPointB.x, diagonalPointB.y );
    cairo_close_path( currentContext );
    flushPath();

    isElementAdded = true;
}


void CAIRO_GAL::DrawPolygon( const SHAPE_POLY_SET& aPolySet )
{
    for( int i = 0; i < aPolySet.OutlineCount(); ++i )
        drawPoly( aPolySet.COutline( i ) );
}


void CAIRO_GAL::DrawCurve( const VECTOR2D& aStartPoint, const VECTOR2D& aControlPointA,
                           const VECTOR2D& aControlPointB, const VECTOR2D& aEndPoint )
{
    cairo_move_to( currentContext, aStartPoint.x, aStartPoint.y );
    cairo_curve_to( currentContext, aControlPointA.x, aControlPointA.y, aControlPointB.x,
                    aControlPointB.y, aEndPoint.x, aEndPoint.y );
    cairo_line_to( currentContext, aEndPoint.x, aEndPoint.y );

    flushPath();
    isElementAdded = true;
}


void CAIRO_GAL::ResizeScreen( int aWidth, int aHeight )
{
    screenSize = VECTOR2I( aWidth, aHeight );

    // Recreate the bitmaps
    deleteBitmaps();
    allocateBitmaps();

    if( validCompositor )
        compositor->Resize( aWidth, aHeight );

    validCompositor = false;

    SetSize( wxSize( aWidth, aHeight ) );
}


bool CAIRO_GAL::Show( bool aShow )
{
    bool s = wxWindow::Show( aShow );

    if( aShow )
        wxWindow::Raise();

    return s;
}


void CAIRO_GAL::Flush()
{
    storePath();
}


void CAIRO_GAL::ClearScreen( const COLOR4D& aColor )
{
    backgroundColor = aColor;
    cairo_set_source_rgb( currentContext, aColor.r, aColor.g, aColor.b );
    cairo_rectangle( currentContext, 0.0, 0.0, screenSize.x, screenSize.y );
    cairo_fill( currentContext );
}


void CAIRO_GAL::SetIsFill( bool aIsFillEnabled )
{
    storePath();
    isFillEnabled = aIsFillEnabled;

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_SET_FILL;
        groupElement.argument.boolArg = aIsFillEnabled;
        currentGroup->push_back( groupElement );
    }
}


void CAIRO_GAL::SetIsStroke( bool aIsStrokeEnabled )
{
    storePath();
    isStrokeEnabled = aIsStrokeEnabled;

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_SET_STROKE;
        groupElement.argument.boolArg = aIsStrokeEnabled;
        currentGroup->push_back( groupElement );
    }
}


void CAIRO_GAL::SetStrokeColor( const COLOR4D& aColor )
{
    storePath();
    strokeColor = aColor;

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_SET_STROKECOLOR;
        groupElement.argument.dblArg[0] = strokeColor.r;
        groupElement.argument.dblArg[1] = strokeColor.g;
        groupElement.argument.dblArg[2] = strokeColor.b;
        groupElement.argument.dblArg[3] = strokeColor.a;
        currentGroup->push_back( groupElement );
    }
}


void CAIRO_GAL::SetFillColor( const COLOR4D& aColor )
{
    storePath();
    fillColor = aColor;

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_SET_FILLCOLOR;
        groupElement.argument.dblArg[0] = fillColor.r;
        groupElement.argument.dblArg[1] = fillColor.g;
        groupElement.argument.dblArg[2] = fillColor.b;
        groupElement.argument.dblArg[3] = fillColor.a;
        currentGroup->push_back( groupElement );
    }
}


void CAIRO_GAL::SetLineWidth( double aLineWidth )
{
    storePath();

    lineWidth = aLineWidth;

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_SET_LINE_WIDTH;
        groupElement.argument.dblArg[0] = aLineWidth;
        currentGroup->push_back( groupElement );
    }
    else
    {
        // Make lines appear at least 1 pixel wide, no matter of zoom
        double x = 1.0, y = 1.0;
        cairo_device_to_user_distance( currentContext, &x, &y );
        double minWidth = std::min( fabs( x ), fabs( y ) );
        cairo_set_line_width( currentContext, std::max( aLineWidth, minWidth ) );
    }
}


void CAIRO_GAL::SetLayerDepth( double aLayerDepth )
{
    super::SetLayerDepth( aLayerDepth );

    if( isInitialized )
        storePath();
}


void CAIRO_GAL::Transform( const MATRIX3x3D& aTransformation )
{
    cairo_matrix_t cairoTransformation;

    cairo_matrix_init( &cairoTransformation,
                       aTransformation.m_data[0][0],
                       aTransformation.m_data[1][0],
                       aTransformation.m_data[0][1],
                       aTransformation.m_data[1][1],
                       aTransformation.m_data[0][2],
                       aTransformation.m_data[1][2] );

    cairo_transform( currentContext, &cairoTransformation );
}


void CAIRO_GAL::Rotate( double aAngle )
{
    storePath();

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_ROTATE;
        groupElement.argument.dblArg[0] = aAngle;
        currentGroup->push_back( groupElement );
    }
    else
    {
        cairo_rotate( currentContext, aAngle );
    }
}


void CAIRO_GAL::Translate( const VECTOR2D& aTranslation )
{
    storePath();

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_TRANSLATE;
        groupElement.argument.dblArg[0] = aTranslation.x;
        groupElement.argument.dblArg[1] = aTranslation.y;
        currentGroup->push_back( groupElement );
    }
    else
    {
        cairo_translate( currentContext, aTranslation.x, aTranslation.y );
    }
}


void CAIRO_GAL::Scale( const VECTOR2D& aScale )
{
    storePath();

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_SCALE;
        groupElement.argument.dblArg[0] = aScale.x;
        groupElement.argument.dblArg[1] = aScale.y;
        currentGroup->push_back( groupElement );
    }
    else
    {
        cairo_scale( currentContext, aScale.x, aScale.y );
    }
}


void CAIRO_GAL::Save()
{
    storePath();

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_SAVE;
        currentGroup->push_back( groupElement );
    }
    else
    {
        cairo_save( currentContext );
    }
}


void CAIRO_GAL::Restore()
{
    storePath();

    if( isGrouping )
    {
        GROUP_ELEMENT groupElement;
        groupElement.command = CMD_RESTORE;
        currentGroup->push_back( groupElement );
    }
    else
    {
        cairo_restore( currentContext );
    }
}


int CAIRO_GAL::BeginGroup()
{
    initSurface();

    // If the grouping is started: the actual path is stored in the group, when
    // a attribute was changed or when grouping stops with the end group method.
    storePath();

    GROUP group;
    int groupNumber = getNewGroupNumber();
    groups.insert( std::make_pair( groupNumber, group ) );
    currentGroup = &groups[groupNumber];
    isGrouping   = true;

    return groupNumber;
}


void CAIRO_GAL::EndGroup()
{
    storePath();
    isGrouping = false;

    deinitSurface();
}


void CAIRO_GAL::DrawGroup( int aGroupNumber )
{
    // This method implements a small Virtual Machine - all stored commands
    // are executed; nested calling is also possible

    storePath();

    for( GROUP::iterator it = groups[aGroupNumber].begin();
         it != groups[aGroupNumber].end(); ++it )
    {
        switch( it->command )
        {
        case CMD_SET_FILL:
            isFillEnabled = it->argument.boolArg;
            break;

        case CMD_SET_STROKE:
            isStrokeEnabled = it->argument.boolArg;
            break;

        case CMD_SET_FILLCOLOR:
            fillColor = COLOR4D( it->argument.dblArg[0], it->argument.dblArg[1], it->argument.dblArg[2],
                                 it->argument.dblArg[3] );
            break;

        case CMD_SET_STROKECOLOR:
            strokeColor = COLOR4D( it->argument.dblArg[0], it->argument.dblArg[1], it->argument.dblArg[2],
                                   it->argument.dblArg[3] );
            break;

        case CMD_SET_LINE_WIDTH:
            {
                // Make lines appear at least 1 pixel wide, no matter of zoom
                double x = 1.0, y = 1.0;
                cairo_device_to_user_distance( currentContext, &x, &y );
                double minWidth = std::min( fabs( x ), fabs( y ) );
                cairo_set_line_width( currentContext, std::max( it->argument.dblArg[0], minWidth ) );
            }
            break;


        case CMD_STROKE_PATH:
            cairo_set_source_rgb( currentContext, strokeColor.r, strokeColor.g, strokeColor.b );
            cairo_append_path( currentContext, it->cairoPath );
            cairo_stroke( currentContext );
            break;

        case CMD_FILL_PATH:
            cairo_set_source_rgb( currentContext, fillColor.r, fillColor.g, fillColor.b );
            cairo_append_path( currentContext, it->cairoPath );
            cairo_fill( currentContext );
            break;

            /*
        case CMD_TRANSFORM:
            cairo_matrix_t matrix;
            cairo_matrix_init( &matrix, it->argument.dblArg[0], it->argument.dblArg[1], it->argument.dblArg[2],
                               it->argument.dblArg[3], it->argument.dblArg[4], it->argument.dblArg[5] );
            cairo_transform( currentContext, &matrix );
            break;
            */

        case CMD_ROTATE:
            cairo_rotate( currentContext, it->argument.dblArg[0] );
            break;

        case CMD_TRANSLATE:
            cairo_translate( currentContext, it->argument.dblArg[0], it->argument.dblArg[1] );
            break;

        case CMD_SCALE:
            cairo_scale( currentContext, it->argument.dblArg[0], it->argument.dblArg[1] );
            break;

        case CMD_SAVE:
            cairo_save( currentContext );
            break;

        case CMD_RESTORE:
            cairo_restore( currentContext );
            break;

        case CMD_CALL_GROUP:
            DrawGroup( it->argument.intArg );
            break;
        }
    }
}


void CAIRO_GAL::ChangeGroupColor( int aGroupNumber, const COLOR4D& aNewColor )
{
    storePath();

    for( GROUP::iterator it = groups[aGroupNumber].begin();
         it != groups[aGroupNumber].end(); ++it )
    {
        if( it->command == CMD_SET_FILLCOLOR || it->command == CMD_SET_STROKECOLOR )
        {
            it->argument.dblArg[0] = aNewColor.r;
            it->argument.dblArg[1] = aNewColor.g;
            it->argument.dblArg[2] = aNewColor.b;
            it->argument.dblArg[3] = aNewColor.a;
        }
    }
}


void CAIRO_GAL::ChangeGroupDepth( int aGroupNumber, int aDepth )
{
    // Cairo does not have any possibilities to change the depth coordinate of stored items,
    // it depends only on the order of drawing
}


void CAIRO_GAL::DeleteGroup( int aGroupNumber )
{
    storePath();

    // Delete the Cairo paths
    std::deque<GROUP_ELEMENT>::iterator it, end;

    for( it = groups[aGroupNumber].begin(), end = groups[aGroupNumber].end(); it != end; ++it )
    {
        if( it->command == CMD_FILL_PATH || it->command == CMD_STROKE_PATH )
        {
            cairo_path_destroy( it->cairoPath );
        }
    }

    // Delete the group
    groups.erase( aGroupNumber );
}


void CAIRO_GAL::ClearCache()
{
    for( int i = groups.size() - 1; i >= 0; --i )
    {
        DeleteGroup( i );
    }
}


void CAIRO_GAL::SaveScreen()
{
    // Copy the current bitmap to the backup buffer
    int offset = 0;

    for( int j = 0; j < screenSize.y; j++ )
    {
        for( int i = 0; i < stride; i++ )
        {
            bitmapBufferBackup[offset + i] = bitmapBuffer[offset + i];
            offset += stride;
        }
    }
}


void CAIRO_GAL::RestoreScreen()
{
    int offset = 0;

    for( int j = 0; j < screenSize.y; j++ )
    {
        for( int i = 0; i < stride; i++ )
        {
            bitmapBuffer[offset + i] = bitmapBufferBackup[offset + i];
            offset += stride;
        }
    }
}


void CAIRO_GAL::SetTarget( RENDER_TARGET aTarget )
{
    // If the compositor is not set, that means that there is a recaching process going on
    // and we do not need the compositor now
    if( !validCompositor )
        return;

    // Cairo grouping prevents display of overlapping items on the same layer in the lighter color
    if( isInitialized )
        storePath();

    switch( aTarget )
    {
    default:
    case TARGET_CACHED:
    case TARGET_NONCACHED:
        compositor->SetBuffer( mainBuffer );
        break;

    case TARGET_OVERLAY:
        compositor->SetBuffer( overlayBuffer );
        break;
    }

    currentTarget = aTarget;
}


RENDER_TARGET CAIRO_GAL::GetTarget() const
{
    return currentTarget;
}


void CAIRO_GAL::ClearTarget( RENDER_TARGET aTarget )
{
    // Save the current state
    unsigned int currentBuffer = compositor->GetBuffer();

    switch( aTarget )
    {
    // Cached and noncached items are rendered to the same buffer
    default:
    case TARGET_CACHED:
    case TARGET_NONCACHED:
        compositor->SetBuffer( mainBuffer );
        break;

    case TARGET_OVERLAY:
        compositor->SetBuffer( overlayBuffer );
        break;
    }

    compositor->ClearBuffer();

    // Restore the previous state
    compositor->SetBuffer( currentBuffer );
}


void CAIRO_GAL::DrawCursor( const VECTOR2D& aCursorPosition )
{
    cursorPosition = aCursorPosition;
}


void CAIRO_GAL::drawGridLine( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint )
{
    cairo_move_to( currentContext, aStartPoint.x, aStartPoint.y );
    cairo_line_to( currentContext, aEndPoint.x, aEndPoint.y );
    cairo_set_source_rgba( currentContext, strokeColor.r, strokeColor.g, strokeColor.b, strokeColor.a );
    cairo_stroke( currentContext );
}


void CAIRO_GAL::flushPath()
{
        if( isFillEnabled )
        {
            cairo_set_source_rgba( currentContext,
                    fillColor.r, fillColor.g, fillColor.b, fillColor.a );

            if( isStrokeEnabled )
                cairo_fill_preserve( currentContext );
            else
                cairo_fill( currentContext );
        }

        if( isStrokeEnabled )
        {
            cairo_set_source_rgba( currentContext,
                    strokeColor.r, strokeColor.g, strokeColor.b, strokeColor.a );
            cairo_stroke( currentContext );
        }
}


void CAIRO_GAL::storePath()
{
    if( isElementAdded )
    {
        isElementAdded = false;

        if( !isGrouping )
        {
            if( isFillEnabled )
            {
                cairo_set_source_rgb( currentContext, fillColor.r, fillColor.g, fillColor.b );
                cairo_fill_preserve( currentContext );
            }

            if( isStrokeEnabled )
            {
                cairo_set_source_rgb( currentContext, strokeColor.r, strokeColor.g,
                                      strokeColor.b );
                cairo_stroke_preserve( currentContext );
            }
        }
        else
        {
            // Copy the actual path, append it to the global path list
            // then check, if the path needs to be stroked/filled and
            // add this command to the group list;
            if( isStrokeEnabled )
            {
                GROUP_ELEMENT groupElement;
                groupElement.cairoPath = cairo_copy_path( currentContext );
                groupElement.command   = CMD_STROKE_PATH;
                currentGroup->push_back( groupElement );
            }

            if( isFillEnabled )
            {
                GROUP_ELEMENT groupElement;
                groupElement.cairoPath = cairo_copy_path( currentContext );
                groupElement.command   = CMD_FILL_PATH;
                currentGroup->push_back( groupElement );
            }
        }

        cairo_new_path( currentContext );
    }
}


void CAIRO_GAL::onPaint( wxPaintEvent& WXUNUSED( aEvent ) )
{
    PostPaint();
}


void CAIRO_GAL::skipMouseEvent( wxMouseEvent& aEvent )
{
    // Post the mouse event to the event listener registered in constructor, if any
    if( mouseListener )
        wxPostEvent( mouseListener, aEvent );
}


void CAIRO_GAL::blitCursor( wxMemoryDC& clientDC )
{
    if( !IsCursorEnabled() )
        return;

    auto p = ToScreen( cursorPosition );

    const auto cColor = getCursorColor();
    const int cursorSize = fullscreenCursor ? 8000 : 80;

    wxColour color( cColor.r * cColor.a * 255, cColor.g * cColor.a * 255,
                    cColor.b * cColor.a * 255, 255 );
    clientDC.SetPen( wxPen( color ) );
    clientDC.DrawLine( p.x - cursorSize / 2, p.y, p.x + cursorSize / 2, p.y );
    clientDC.DrawLine( p.x, p.y - cursorSize / 2, p.x, p.y + cursorSize / 2 );
}


void CAIRO_GAL::allocateBitmaps()
{
    wxBufferWidth = screenSize.x;
    while( ( ( wxBufferWidth * 3 ) % 4 ) != 0 ) wxBufferWidth++;

    // Create buffer, use the system independent Cairo context backend
    stride     = cairo_format_stride_for_width( GAL_FORMAT, wxBufferWidth );
    bufferSize = stride * screenSize.y;

    bitmapBuffer        = new unsigned int[bufferSize];
    bitmapBufferBackup  = new unsigned int[bufferSize];
    wxOutput            = new unsigned char[wxBufferWidth * 3 * screenSize.y];
}


void CAIRO_GAL::deleteBitmaps()
{
    delete[] bitmapBuffer;
    delete[] bitmapBufferBackup;
    delete[] wxOutput;
}


void CAIRO_GAL::initSurface()
{
    if( isInitialized )
        return;

    // Create the Cairo surface
    surface = cairo_image_surface_create_for_data( (unsigned char*) bitmapBuffer, GAL_FORMAT,
                                                   wxBufferWidth, screenSize.y, stride );
    context = cairo_create( surface );
#ifdef __WXDEBUG__
    cairo_status_t status = cairo_status( context );
    wxASSERT_MSG( status == CAIRO_STATUS_SUCCESS, wxT( "Cairo context creation error" ) );
#endif /* __WXDEBUG__ */
    currentContext = context;

    cairo_set_antialias( context, CAIRO_ANTIALIAS_NONE );

    // Clear the screen
    ClearScreen( backgroundColor );

    // Compute the world <-> screen transformations
    ComputeWorldScreenMatrix();

    cairo_matrix_init( &cairoWorldScreenMatrix, worldScreenMatrix.m_data[0][0],
                       worldScreenMatrix.m_data[1][0], worldScreenMatrix.m_data[0][1],
                       worldScreenMatrix.m_data[1][1], worldScreenMatrix.m_data[0][2],
                       worldScreenMatrix.m_data[1][2] );

    cairo_set_matrix( context, &cairoWorldScreenMatrix );

    // Start drawing with a new path
    cairo_new_path( context );
    isElementAdded = true;

    cairo_set_line_join( context, CAIRO_LINE_JOIN_ROUND );
    cairo_set_line_cap( context, CAIRO_LINE_CAP_ROUND );

    lineWidth = 0;

    isInitialized = true;
}


void CAIRO_GAL::deinitSurface()
{
    if( !isInitialized )
        return;

    // Destroy Cairo objects
    cairo_destroy( context );
    cairo_surface_destroy( surface );

    isInitialized = false;
}


void CAIRO_GAL::setCompositor()
{
    // Recreate the compositor with the new Cairo context
    compositor.reset( new CAIRO_COMPOSITOR( &currentContext ) );
    compositor->Resize( screenSize.x, screenSize.y );

    // Prepare buffers
    mainBuffer = compositor->CreateBuffer();
    overlayBuffer = compositor->CreateBuffer();

    validCompositor = true;
}


void CAIRO_GAL::drawPoly( const std::deque<VECTOR2D>& aPointList )
{
    // Iterate over the point list and draw the segments
    std::deque<VECTOR2D>::const_iterator it = aPointList.begin();

    cairo_move_to( currentContext, it->x, it->y );

    for( ++it; it != aPointList.end(); ++it )
    {
        cairo_line_to( currentContext, it->x, it->y );
    }

    flushPath();
    isElementAdded = true;
}


void CAIRO_GAL::drawPoly( const VECTOR2D aPointList[], int aListSize )
{
    // Iterate over the point list and draw the segments
    const VECTOR2D* ptr = aPointList;

    cairo_move_to( currentContext, ptr->x, ptr->y );

    for( int i = 0; i < aListSize; ++i )
    {
        ++ptr;
        cairo_line_to( currentContext, ptr->x, ptr->y );
    }

    flushPath();
    isElementAdded = true;
}


void CAIRO_GAL::drawPoly( const SHAPE_LINE_CHAIN& aLineChain )
{
    if( aLineChain.PointCount() < 2 )
        return;

    auto numPoints = aLineChain.PointCount();

    if( aLineChain.IsClosed() )
        numPoints += 1;

    const VECTOR2I start = aLineChain.CPoint( 0 );
    cairo_move_to( currentContext, start.x, start.y );

    for( int i = 1; i < numPoints; ++i )
    {
        const VECTOR2I& p = aLineChain.CPoint( i );
        cairo_line_to( currentContext, p.x, p.y );
    }

    flushPath();
    isElementAdded = true;
}


unsigned int CAIRO_GAL::getNewGroupNumber()
{
    wxASSERT_MSG( groups.size() < std::numeric_limits<unsigned int>::max(),
                  wxT( "There are no free slots to store a group" ) );

    while( groups.find( groupCounter ) != groups.end() )
        groupCounter++;

    return groupCounter++;
}
