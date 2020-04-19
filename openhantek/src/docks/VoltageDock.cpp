// SPDX-License-Identifier: GPL-2.0+

#include <QCloseEvent>
#include <QDebug>
#include <QDockWidget>
#include <QSignalBlocker>

#include <cmath>

#include "VoltageDock.h"
#include "dockwindows.h"

#include "dsosettings.h"
#include "sispinbox.h"
#include "utils/printutils.h"
#include "viewconstants.h"

template <typename... Args> struct SELECT {
    template <typename C, typename R> static constexpr auto OVERLOAD_OF( R ( C::*pmf )( Args... ) ) -> decltype( pmf ) {
        return pmf;
    }
};

VoltageDock::VoltageDock( DsoSettingsScope *scope, const Dso::ControlSpecification *spec, QWidget *parent,
                          Qt::WindowFlags flags )
    : QDockWidget( tr( "Voltage" ), parent, flags ), scope( scope ), spec( spec ) {

    // Initialize lists for comboboxes
    for ( Dso::Coupling c : spec->couplings )
        couplingStrings.append( Dso::couplingString( c ) );

    for ( auto e : Dso::MathModeEnum ) {
        modeStrings.append( Dso::mathModeString( e ) );
    }

    for ( double gainStep : scope->gainSteps ) {
        gainStrings << valueToString( gainStep, UNIT_VOLTS, 0 );
    }

    dockLayout = new QGridLayout();
    dockLayout->setColumnMinimumWidth( 0, 64 );
    dockLayout->setColumnStretch( 1, 1 ); // stretch ComboBox in 2nd (middle) column
    dockLayout->setColumnStretch( 2, 1 ); // stretch ComboBox in 3rd (last) column
    dockLayout->setSpacing( DOCK_LAYOUT_SPACING );
    // Initialize elements
    int row = 0;
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        ChannelBlock b;

        if ( channel < spec->channels )
            b.usedCheckBox = new QCheckBox( tr( "CH&%1" ).arg( channel + 1 ) ); // define shortcut <ALT>1 / <ALT>2
        else
            b.usedCheckBox = new QCheckBox( tr( "&MATH" ) );
        b.miscComboBox = new QComboBox();
        b.gainComboBox = new QComboBox();
        b.invertCheckBox = new QCheckBox( tr( "Invert" ) );
        b.attnSpinBox = new QSpinBox();
        b.attnSpinBox->setMinimum( ATTENUATION_MIN );
        b.attnSpinBox->setMaximum( ATTENUATION_MAX );
        b.attnSpinBox->setPrefix( tr( "x" ) );

        channelBlocks.push_back( std::move( b ) );

        if ( channel < spec->channels )
            b.miscComboBox->addItems( couplingStrings );
        else
            b.miscComboBox->addItems( modeStrings );

        b.gainComboBox->addItems( gainStrings );

        dockLayout->addWidget( b.usedCheckBox, row, 0 );
        dockLayout->addWidget( b.gainComboBox, row++, 1, 1, 2 );
        dockLayout->addWidget( b.invertCheckBox, row, 0 );
        if ( channel < spec->channels ) {
            dockLayout->addWidget( b.attnSpinBox, row, 1, 1, 1 );
            dockLayout->addWidget( b.miscComboBox, row++, 2, 1, 1 );
        } else {
            dockLayout->addWidget( b.miscComboBox, row++, 1, 1, 2 );
        }

        // draw divider line
        if ( channel < spec->channels ) {
            QFrame *divider = new QFrame();
            divider->setLineWidth( 1 );
            divider->setFrameShape( QFrame::HLine );
            dockLayout->addWidget( divider, row++, 0, 1, 3 );
        }

        connect( b.gainComboBox, SELECT<int>::OVERLOAD_OF( &QComboBox::currentIndexChanged ),
                 [this, channel]( unsigned index ) {
                     this->scope->voltage[ channel ].gainStepIndex = index;
                     emit gainChanged( channel, this->scope->gain( channel ) );
                 } );
        connect( b.attnSpinBox, SELECT<int>::OVERLOAD_OF( &QSpinBox::valueChanged ),
                 [this, channel]( unsigned attnValue ) {
                     this->scope->voltage[ channel ].probeAttn = attnValue;
                     setAttn( channel, attnValue );
                     emit probeAttnChanged(
                         channel, attnValue ); // make sure to set the probe first, since this will influence the gain
                     emit gainChanged( channel, this->scope->gain( channel ) );
                 } );
        connect( b.invertCheckBox, &QAbstractButton::toggled, [this, channel]( bool checked ) {
            this->scope->voltage[ channel ].inverted = checked;
            emit invertedChanged( channel, checked );
        } );
        connect( b.miscComboBox, SELECT<int>::OVERLOAD_OF( &QComboBox::currentIndexChanged ),
                 [this, channel, spec, scope]( unsigned index ) {
                     this->scope->voltage[ channel ].couplingOrMathIndex = index;
                     if ( channel < spec->channels ) {
                         // setCoupling(channel, (unsigned)index);
                         emit couplingChanged( channel, scope->coupling( channel, spec ) );
                     } else {
                         emit modeChanged( Dso::getMathMode( this->scope->voltage[ channel ] ) );
                     }
                 } );
        connect( b.usedCheckBox, &QAbstractButton::toggled, [this, channel]( bool checked ) {
            this->scope->voltage[ channel ].used = checked;
            emit usedChanged( channel, checked );
        } );
    }

    // Load settings into GUI
    this->loadSettings( scope, spec );

    dockWidget = new QWidget();
    SetupDockWidget( this, dockWidget, dockLayout );
}

void VoltageDock::loadSettings( DsoSettingsScope *scope, const Dso::ControlSpecification *spec ) {
    for ( ChannelID channel = 0; channel < scope->voltage.size(); ++channel ) {
        if ( channel < spec->channels ) {
            if ( int( scope->voltage[ channel ].couplingOrMathIndex ) < couplingStrings.size() )
                setCoupling( channel, scope->voltage[ channel ].couplingOrMathIndex );
        } else {
            setMode( scope->voltage[ channel ].couplingOrMathIndex );
        }

        setGain( channel, scope->voltage[ channel ].gainStepIndex );
        setUsed( channel, scope->voltage[ channel ].used );
        setAttn( channel, scope->voltage[ channel ].probeAttn );
        setInverted( channel, scope->voltage[ channel ].inverted );
    }
}

/// \brief Don't close the dock, just hide it
/// \param event The close event that should be handled.
void VoltageDock::closeEvent( QCloseEvent *event ) {
    hide();
    event->accept();
}

void VoltageDock::setCoupling( ChannelID channel, unsigned couplingIndex ) {
    if ( channel >= spec->channels )
        return;
    if ( couplingIndex >= spec->couplings.size() )
        return;
    QSignalBlocker blocker( channelBlocks[ channel ].miscComboBox );
    channelBlocks[ channel ].miscComboBox->setCurrentIndex( int( couplingIndex ) );
}

void VoltageDock::setGain( ChannelID channel, unsigned gainStepIndex ) {
    if ( channel >= scope->voltage.size() )
        return;
    if ( gainStepIndex >= scope->gainSteps.size() )
        return;
    QSignalBlocker blocker( channelBlocks[ channel ].gainComboBox );
    channelBlocks[ channel ].gainComboBox->setCurrentIndex( int( gainStepIndex ) );
}

void VoltageDock::setAttn( ChannelID channel, double attnValue ) {
    if ( channel >= scope->voltage.size() )
        return;
    QSignalBlocker blocker( channelBlocks[ channel ].gainComboBox );
    int index = channelBlocks[ channel ].gainComboBox->currentIndex();
    gainStrings.clear();
    for ( double gainStep : scope->gainSteps ) {
        gainStrings << valueToString( gainStep * attnValue, UNIT_VOLTS, -1 ); // auto format
    }
    channelBlocks[ channel ].gainComboBox->clear();
    channelBlocks[ channel ].gainComboBox->addItems( gainStrings );
    channelBlocks[ channel ].gainComboBox->setCurrentIndex( index );
    scope->voltage[ channel ].probeAttn = attnValue;
    channelBlocks[ channel ].attnSpinBox->setValue( int( attnValue ) );
}

void VoltageDock::setMode( unsigned mathModeIndex ) {
    QSignalBlocker blocker( channelBlocks[ spec->channels ].miscComboBox );
    channelBlocks[ spec->channels ].miscComboBox->setCurrentIndex( int( mathModeIndex ) );
}

void VoltageDock::setUsed( ChannelID channel, bool used ) {
    if ( channel >= scope->voltage.size() )
        return;
    QSignalBlocker blocker( channelBlocks[ channel ].usedCheckBox );
    channelBlocks[ channel ].usedCheckBox->setChecked( used );
}

void VoltageDock::setInverted( ChannelID channel, bool inverted ) {
    if ( channel >= scope->voltage.size() )
        return;
    QSignalBlocker blocker( channelBlocks[ channel ].invertCheckBox );
    channelBlocks[ channel ].invertCheckBox->setChecked( inverted );
}
