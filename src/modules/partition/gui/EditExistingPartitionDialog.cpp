/* === This file is part of Calamares - <http://github.com/calamares> ===
 *
 *   Copyright 2014, Aurélien Gâteau <agateau@kde.org>
 *
 *   Calamares is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Calamares is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Calamares. If not, see <http://www.gnu.org/licenses/>.
 */

#include <gui/EditExistingPartitionDialog.h>

#include <core/ColorUtils.h>
#include <core/PartitionCoreModule.h>
#include <core/PartitionInfo.h>
#include <core/KPMHelpers.h>
#include <gui/PartitionSizeController.h>

#include <ui_EditExistingPartitionDialog.h>

#include <utils/Logger.h>
#include "GlobalStorage.h"
#include "JobQueue.h"

// KPMcore
#include <kpmcore/core/device.h>
#include <kpmcore/core/partition.h>
#include <kpmcore/fs/filesystemfactory.h>

// Qt
#include <QComboBox>
#include <QDir>

EditExistingPartitionDialog::EditExistingPartitionDialog( Device* device, Partition* partition, QWidget* parentWidget )
    : QDialog( parentWidget )
    , m_ui( new Ui_EditExistingPartitionDialog )
    , m_device( device )
    , m_partition( partition )
    , m_partitionSizeController( new PartitionSizeController( this ) )
{
    m_ui->setupUi( this );

    QStringList mountPoints = { "/", "/boot", "/home", "/opt", "/usr", "/var" };
    if ( QDir( "/sys/firmware/efi/efivars" ).exists() )
        mountPoints << Calamares::JobQueue::instance()->globalStorage()->value( "efiSystemPartition" ).toString();
    mountPoints.removeDuplicates();
    mountPoints.sort();
    m_ui->mountPointComboBox->addItems( mountPoints );

    QColor color = ColorUtils::colorForPartition( m_partition );
    m_partitionSizeController->init( m_device, m_partition, color );
    m_partitionSizeController->setSpinBox( m_ui->sizeSpinBox );

    m_ui->mountPointComboBox->setCurrentText( PartitionInfo::mountPoint( partition ) );

    replacePartResizerWidget();

    connect( m_ui->formatRadioButton, &QAbstractButton::toggled,
             [ this ]( bool doFormat )
    {
        replacePartResizerWidget();

        m_ui->fileSystemLabel->setEnabled( doFormat );
        m_ui->fileSystemComboBox->setEnabled( doFormat );

        if ( !doFormat )
            m_ui->fileSystemComboBox->setCurrentText( m_partition->fileSystem().name() );

        updateMountPointPicker();
    } );

    connect( m_ui->fileSystemComboBox, &QComboBox::currentTextChanged,
             [ this ]( QString )
    {
        updateMountPointPicker();
    } );

    // File system
    QStringList fsNames;
    for ( auto fs : FileSystemFactory::map() )
    {
        if ( fs->supportCreate() != FileSystem::cmdSupportNone && fs->type() != FileSystem::Extended )
            fsNames << fs->name();
    }
    m_ui->fileSystemComboBox->addItems( fsNames );

    if ( fsNames.contains( m_partition->fileSystem().name() ) )
        m_ui->fileSystemComboBox->setCurrentText( m_partition->fileSystem().name() );
    else
        m_ui->fileSystemComboBox->setCurrentText( FileSystem::nameForType( FileSystem::Ext4 ) );

    m_ui->fileSystemLabel->setEnabled( m_ui->formatRadioButton->isChecked() );
    m_ui->fileSystemComboBox->setEnabled( m_ui->formatRadioButton->isChecked() );
}

EditExistingPartitionDialog::~EditExistingPartitionDialog()
{}

void
EditExistingPartitionDialog::applyChanges( PartitionCoreModule* core )
{
    PartitionInfo::setMountPoint( m_partition, m_ui->mountPointComboBox->currentText() );

    qint64 newFirstSector = m_partitionSizeController->firstSector();
    qint64 newLastSector = m_partitionSizeController->lastSector();
    bool partitionChanged = newFirstSector != m_partition->firstSector() || newLastSector != m_partition->lastSector();

    FileSystem::Type fsType = FileSystem::Unknown;
    if ( m_ui->formatRadioButton->isChecked() )
    {
        fsType = m_partition->roles().has( PartitionRole::Extended )
                ? FileSystem::Extended
                : FileSystem::typeForName( m_ui->fileSystemComboBox->currentText() );
    }

    if ( partitionChanged )
    {
        if ( m_ui->formatRadioButton->isChecked() )
        {
            Partition* newPartition = KPMHelpers::createNewPartition(
                                          m_partition->parent(),
                                          *m_device,
                                          m_partition->roles(),
                                          fsType,
                                          newFirstSector,
                                          newLastSector );
            PartitionInfo::setMountPoint( newPartition, PartitionInfo::mountPoint( m_partition ) );
            PartitionInfo::setFormat( newPartition, true );

            core->deletePartition( m_device, m_partition );
            core->createPartition( m_device, newPartition );
        }
        else
        {
            core->resizePartition( m_device,
                                   m_partition,
                                   newFirstSector,
                                   newLastSector );
        }
    }
    else
    {
        // No size changes
        if ( m_ui->formatRadioButton->isChecked() )
        {
            // if the FS type is unchanged, we just format
            if ( m_partition->fileSystem().type() == fsType )
            {
                core->formatPartition( m_device, m_partition );
            }
            else // otherwise, we delete and recreate the partition with new fs type
            {
                Partition* newPartition = KPMHelpers::createNewPartition(
                                              m_partition->parent(),
                                              *m_device,
                                              m_partition->roles(),
                                              fsType,
                                              m_partition->firstSector(),
                                              m_partition->lastSector() );
                PartitionInfo::setMountPoint( newPartition, PartitionInfo::mountPoint( m_partition ) );
                PartitionInfo::setFormat( newPartition, true );

                core->deletePartition( m_device, m_partition );
                core->createPartition( m_device, newPartition );
            }
        }
        else
        {
            core->refreshPartition( m_device, m_partition );
        }
    }
}

void
EditExistingPartitionDialog::replacePartResizerWidget()
{
    /*
     * There is no way to reliably update the partition used by
     * PartResizerWidget, which is necessary when we switch between "format" and
     * "keep". This is a hack which replaces the existing PartResizerWidget
     * with a new one.
     */
    PartResizerWidget* widget = new PartResizerWidget( this );

    layout()->replaceWidget( m_ui->partResizerWidget, widget );
    delete m_ui->partResizerWidget;
    m_ui->partResizerWidget = widget;

    m_partitionSizeController->setPartResizerWidget( widget, m_ui->formatRadioButton->isChecked() );
}

void
EditExistingPartitionDialog::updateMountPointPicker()
{
    bool doFormat = m_ui->formatRadioButton->isChecked();
    FileSystem::Type fsType = FileSystem::Unknown;
    if ( doFormat )
    {
        fsType = FileSystem::typeForName( m_ui->fileSystemComboBox->currentText() );
    }
    else
    {
        fsType = m_partition->fileSystem().type();
    }
    bool canMount = true;
    if ( fsType == FileSystem::Extended ||
         fsType == FileSystem::LinuxSwap ||
         fsType == FileSystem::Unformatted ||
         fsType == FileSystem::Unknown ||
         fsType == FileSystem::Lvm2_PV )
    {
        canMount = false;
    }

    m_ui->mountPointLabel->setEnabled( canMount );
    m_ui->mountPointComboBox->setEnabled( canMount );
    if ( !canMount )
        m_ui->mountPointComboBox->setCurrentText( QString() );
}
