#include <QSplitter>
#include <QThread>
#include "reconstruction_window.h"
#include "ui_reconstruction_window.h"
#include "mapping/fa_template.hpp"
#include "tipl/tipl.hpp"
#include "mainwindow.h"
#include <QImage>
#include <QMessageBox>
#include <QInputDialog>
#include <QFileDialog>
#include <QSettings>
#include "prog_interface_static_link.h"
#include "tracking/region/Regions.h"
#include "libs/dsi/image_model.hpp"
#include "gzip_interface.hpp"
#include "manual_alignment.h"

extern std::vector<std::string> fa_template_list;
void show_view(QGraphicsScene& scene,QImage I);
bool reconstruction_window::load_src(int index)
{
    begin_prog("load src");
    check_prog(index,filenames.size());
    handle.reset(new ImageModel);
    if (!handle->load_from_file(filenames[index].toLocal8Bit().begin()))
    {
        QMessageBox::information(this,"error",QString("Cannot open ") +
            filenames[index] + " : " +handle->error_msg.c_str(),0);
        check_prog(0,0);
        return false;
    }
    float m = (float)*std::max_element(handle->src_dwi_data[0],handle->src_dwi_data[0]+handle->voxel.dim.size());
    float otsu = tipl::segmentation::otsu_threshold(tipl::make_image(handle->src_dwi_data[0],handle->voxel.dim));
    ui->max_value->setMaximum(m*1.5f);
    ui->max_value->setMinimum(0.0f);
    ui->max_value->setSingleStep(m*0.05f);
    ui->max_value->setValue(otsu*3.0f);
    ui->min_value->setMaximum(m*1.5f);
    ui->min_value->setMinimum(0.0f);
    ui->min_value->setSingleStep(m*0.05f);
    ui->min_value->setValue(0.0f);

    // load all templates for QSDR
    if(!fa_template_list.empty())
    {
        for(int index = 0;index < fa_template_list.size();++index)
            ui->template_box->addItem(QFileInfo(fa_template_list[index].c_str()).baseName());
        ui->template_box->setCurrentIndex(0);
    }
    else
        ui->template_box->hide();


    update_image();
    return true;
}

void calculate_shell(const std::vector<float>& bvalues,std::vector<unsigned int>& shell);
bool is_dsi_half_sphere(const std::vector<unsigned int>& shell);
bool is_dsi(const std::vector<unsigned int>& shell);
bool is_multishell(const std::vector<unsigned int>& shell);
bool need_scheme_balance(const std::vector<unsigned int>& shell);

reconstruction_window::reconstruction_window(QStringList filenames_,QWidget *parent) :
    QMainWindow(parent),filenames(filenames_),ui(new Ui::reconstruction_window)
{
    ui->setupUi(this);
    if(!load_src(0))
        throw std::runtime_error("Cannot load src file");
    ui->ThreadCount->setMaximum(std::thread::hardware_concurrency());
    ui->toolBox->setCurrentIndex(1);
    ui->graphicsView->setScene(&scene);
    ui->view_source->setScene(&source);
    ui->b_table->setColumnWidth(0,60);
    ui->b_table->setColumnWidth(1,80);
    ui->b_table->setColumnWidth(2,80);
    ui->b_table->setColumnWidth(3,80);
    ui->b_table->setHorizontalHeaderLabels(QStringList() << "b value" << "bx" << "by" << "bz");
    ui->gqi_spectral->hide();
    ui->DSI->hide();
    ui->QBI->hide();
    ui->DDI->hide();
    ui->ODFSharpening->setVisible(false);
    ui->ODFSharpening->setEnabled(false);


    v2c.two_color(tipl::rgb(0,0,0),tipl::rgb(255,255,255));
    update_dimension();

    absolute_path = QFileInfo(filenames[0]).absolutePath();


    switch(settings.value("rec_method_id",4).toInt())
    {
    case 1:
        ui->DTI->setChecked(true);
        on_DTI_toggled(true);
        break;
    case 7:
        ui->QSDR->setChecked(true);
        on_QSDR_toggled(true);
        break;
    case 8:
        ui->DDI->setVisible(true);
        ui->DDI->setChecked(true);
        on_DDI_toggled(true);
        break;
    default:
        ui->GQI->setChecked(true);
        on_GQI_toggled(true);
        break;
    }
    ui->AdvancedWidget->setVisible(false);
    ui->ThreadCount->setValue(settings.value("rec_thread_count",std::thread::hardware_concurrency()).toInt());
    ui->NumOfFibers->setValue(settings.value("rec_num_fiber",5).toInt());
    ui->ODFDef->setCurrentIndex(settings.value("rec_gqi_def",0).toInt());
    ui->reg_method->setCurrentIndex(settings.value("rec_reg_method",0).toInt());

    ui->diffusion_sampling->setValue(settings.value("rec_gqi_sampling",1.25).toDouble());
    ui->csf_calibration->setChecked(settings.value("csf_calibration",1).toInt());
    ui->regularization_param->setValue(settings.value("rec_qbi_reg",0.006).toDouble());
    ui->SHOrder->setValue(settings.value("rec_qbi_sh_order",8).toInt());
    ui->hamming_filter->setValue(settings.value("rec_hamming_filter",17).toDouble());

    ui->odf_sharpening->setCurrentIndex(settings.value("rec_odf_sharpening",0).toInt());
    ui->decon_param->setValue(settings.value("rec_deconvolution_param",3.0).toDouble());
    ui->decom_m->setValue(settings.value("rec_decom_m",10).toInt());
    ui->ODFDim->setCurrentIndex(settings.value("odf_order",3).toInt());

    ui->RecordODF->setChecked(settings.value("rec_record_odf",0).toInt());
    ui->output_jacobian->setChecked(settings.value("output_jacobian",0).toInt());
    ui->output_mapping->setChecked(settings.value("output_mapping",0).toInt());
    ui->output_diffusivity->setChecked(settings.value("output_diffusivity",1).toInt());
    ui->output_tensor->setChecked(settings.value("output_tensor",0).toInt());
    ui->rdi->setChecked(settings.value("output_rdi",1).toInt());
    ui->check_btable->setChecked(settings.value("check_btable",1).toInt());

    ui->report->setText(handle->voxel.report.c_str());

    max_source_value = *std::max_element(handle->src_dwi_data.back(),
                                         handle->src_dwi_data.back()+handle->voxel.dim.size());



    on_odf_sharpening_currentIndexChanged(ui->odf_sharpening->currentIndex());
    connect(ui->z_pos,SIGNAL(valueChanged(int)),this,SLOT(on_b_table_itemSelectionChanged()));
    connect(ui->max_value,SIGNAL(valueChanged(double)),this,SLOT(on_b_table_itemSelectionChanged()));
    connect(ui->min_value,SIGNAL(valueChanged(double)),this,SLOT(on_b_table_itemSelectionChanged()));

    on_b_table_itemSelectionChanged();


    {
        ui->half_sphere->setChecked(handle->is_dsi_half_sphere());
        ui->scheme_balance->setChecked(handle->need_scheme_balance());
        if(handle->is_dsi())
            ui->scheme_balance->setEnabled(false);
        else
        // not dsi
        {
            if(ui->DSI->isChecked())
            {
                ui->GQI->setChecked(true);
                on_GQI_toggled(true);
            }
            ui->DSI->setEnabled(false);
            ui->half_sphere->setEnabled(false);
        }
        if(handle->is_dsi() || handle->is_multishell())
        {
            if(ui->QBI->isChecked())
            {
                ui->GQI->setChecked(true);
                on_GQI_toggled(true);
            }
            ui->QBI->setEnabled(false);
        }
    }



}
void reconstruction_window::update_dimension(void)
{
    ui->SlicePos->setRange(0,handle->voxel.dim[2]-1);
    ui->SlicePos->setValue((handle->voxel.dim[2]-1) >> 1);
    ui->z_pos->setRange(0,handle->voxel.dim[2]-1);
    ui->z_pos->setValue((handle->voxel.dim[2]-1) >> 1);
    ui->x->setMaximum(handle->voxel.dim[0]-1);
    ui->y->setMaximum(handle->voxel.dim[1]-1);
    ui->z->setMaximum(handle->voxel.dim[2]-1);
    source_ratio = std::max(1.0,500/(double)handle->voxel.dim.height());
}

void reconstruction_window::load_b_table(void)
{
    ui->b_table->clear();
    ui->b_table->setRowCount(handle->src_bvalues.size());
    for(unsigned int index = 0;index < handle->src_bvalues.size();++index)
    {
        ui->b_table->setItem(index,0, new QTableWidgetItem(QString::number(handle->src_bvalues[index])));
        ui->b_table->setItem(index,1, new QTableWidgetItem(QString::number(handle->src_bvectors[index][0])));
        ui->b_table->setItem(index,2, new QTableWidgetItem(QString::number(handle->src_bvectors[index][1])));
        ui->b_table->setItem(index,3, new QTableWidgetItem(QString::number(handle->src_bvectors[index][2])));
    }
    ui->b_table->selectRow(0);
}
void reconstruction_window::on_b_table_itemSelectionChanged()
{
    v2c.set_range(ui->min_value->value(),ui->max_value->value());
    tipl::image<float,2> tmp(tipl::geometry<2>(handle->voxel.dim[0],handle->voxel.dim[1]));
    unsigned int b_index = ui->b_table->currentRow();
    std::copy(handle->src_dwi_data[b_index] + ui->z_pos->value()*tmp.size(),
              handle->src_dwi_data[b_index] + ui->z_pos->value()*tmp.size() + tmp.size(),tmp.begin());
    buffer_source.resize(tmp.geometry());
    for(int i = 0;i < tmp.size();++i)
        buffer_source[i] = v2c[tmp[i]];
    source_image = QImage((unsigned char*)&*buffer_source.begin(),tmp.width(),tmp.height(),QImage::Format_RGB32).
                    scaled(tmp.width()*source_ratio,tmp.height()*source_ratio);
    show_view(source,source_image);
}


void reconstruction_window::resizeEvent ( QResizeEvent * event )
{
    QMainWindow::resizeEvent(event);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}
void reconstruction_window::showEvent ( QShowEvent * event )
{
    QMainWindow::showEvent(event);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::closeEvent(QCloseEvent *event)
{
    QMainWindow::closeEvent(event);

}

reconstruction_window::~reconstruction_window()
{
    delete ui;
}

void reconstruction_window::doReconstruction(unsigned char method_id,bool prompt)
{
    if(!handle.get())
        return;

    if (*std::max_element(handle->voxel.mask.begin(),handle->voxel.mask.end()) == 0)
    {
        QMessageBox::information(this,"error","Please select mask for reconstruction",0);
        return;
    }

    if (ui->odf_sharpening->currentIndex() == 1 && method_id != 1) // deconvolution
    {
        handle->voxel.param[2] = ui->decon_param->value();
        settings.setValue("rec_deconvolution_param",handle->voxel.param[2]);
    }
    if (ui->odf_sharpening->currentIndex() == 2 && method_id != 1) // decomposition
    {
        handle->voxel.param[3] = ui->decom_fraction->value();
        handle->voxel.param[4] = ui->decom_m->value();
        settings.setValue("rec_decomposition_param",handle->voxel.param[3]);
        settings.setValue("rec_decom_m",handle->voxel.param[4]);
    }
    //T1W DMDM
    if(method_id == 7 && ui->reg_method->currentIndex() == 4)
    {
        QString t1w_file_name1 =
                QFileInfo(handle->file_name.c_str()).absolutePath() + "/" + QFileInfo(handle->file_name.c_str()).baseName() + "_t1w.nii.gz";
        QString t1w_file_name2 =
                QFileInfo(handle->file_name.c_str()).absolutePath() + "/" + QFileInfo(handle->file_name.c_str()).baseName() + "_MPRAGE.nii.gz";
        if(QFileInfo(t1w_file_name1).exists())
            handle->voxel.t1w_file_name = t1w_file_name1.toStdString();
        else
        if(QFileInfo(t1w_file_name2).exists())
            handle->voxel.t1w_file_name = t1w_file_name2.toStdString();
        else
        {
            QMessageBox::information(0,"Reconstruction","Please Assign T1W file for normalization",0);
            QString filename = QFileDialog::getOpenFileName(
                    this,"Open T1W files",absolute_path,
                    "Images (*.nii *nii.gz);;All files (*)" );
            if( filename.isEmpty())
                return;
            handle->voxel.t1w_file_name = filename.toStdString();
        }
     }
    if(method_id == 7 && !fa_template_list.empty() && ui->template_box->currentIndex() != 0)
        handle->voxel.external_template = fa_template_list[ui->template_box->currentIndex()];

    settings.setValue("rec_method_id",method_id);
    settings.setValue("rec_thread_count",ui->ThreadCount->value());
    settings.setValue("rec_odf_sharpening",ui->odf_sharpening->currentIndex());
    settings.setValue("rec_num_fiber",ui->NumOfFibers->value());
    settings.setValue("rec_gqi_def",ui->ODFDef->currentIndex());
    settings.setValue("rec_reg_method",ui->reg_method->currentIndex());
    settings.setValue("csf_calibration",ui->csf_calibration->isChecked() ? 1 : 0);


    settings.setValue("odf_order",ui->ODFDim->currentIndex());
    settings.setValue("rec_record_odf",ui->RecordODF->isChecked() ? 1 : 0);
    settings.setValue("output_jacobian",ui->output_jacobian->isChecked() ? 1 : 0);
    settings.setValue("output_mapping",ui->output_mapping->isChecked() ? 1 : 0);
    settings.setValue("output_diffusivity",ui->output_diffusivity->isChecked() ? 1 : 0);
    settings.setValue("output_tensor",ui->output_tensor->isChecked() ? 1 : 0);
    settings.setValue("output_rdi",(ui->rdi->isChecked() && (method_id == 4 || method_id == 7)) ? 1 : 0); // only for GQI
    settings.setValue("check_btable",ui->check_btable->isChecked() ? 1 : 0);

    begin_prog("reconstruction",true);
    int odf_order[8] = {4, 5, 6, 8, 10, 12, 16, 20};
    handle->voxel.method_id = method_id;
    handle->voxel.ti.init(odf_order[ui->ODFDim->currentIndex()]);
    handle->voxel.odf_deconvolusion = 0;//ui->odf_sharpening->currentIndex() == 1 ? 1 : 0;
    handle->voxel.odf_decomposition = 0;//ui->odf_sharpening->currentIndex() == 2 ? 1 : 0;
    handle->voxel.odf_xyz[0] = ui->x->value();
    handle->voxel.odf_xyz[1] = ui->y->value();
    handle->voxel.odf_xyz[2] = ui->z->value();
    handle->voxel.csf_calibration = (ui->csf_calibration->isVisible() && ui->csf_calibration->isChecked()) ? 1: 0;
    handle->voxel.max_fiber_number = ui->NumOfFibers->value();
    handle->voxel.r2_weighted = ui->ODFDef->currentIndex();
    handle->voxel.reg_method = ui->reg_method->currentIndex();
    handle->voxel.need_odf = ui->RecordODF->isChecked();
    handle->voxel.check_btable = ui->check_btable->isChecked();
    handle->voxel.output_jacobian = ui->output_jacobian->isChecked();
    handle->voxel.output_mapping = ui->output_mapping->isChecked();
    handle->voxel.output_diffusivity = ui->output_diffusivity->isChecked();
    handle->voxel.output_tensor = ui->output_tensor->isChecked();
    handle->voxel.output_rdi = ui->rdi->isChecked();
    handle->voxel.thread_count = ui->ThreadCount->value();


    if(method_id == 7 || method_id == 4 || method_id == 8)
    {
        handle->voxel.half_sphere = ui->half_sphere->isChecked() ? 1:0;
        handle->voxel.scheme_balance = ui->scheme_balance->isChecked() ? 1:0;
    }
    else
    {
        handle->voxel.half_sphere = false;
        handle->voxel.scheme_balance = false;
    }

    const char* msg = handle->reconstruction();
    if (!QFileInfo(msg).exists())
    {
        QMessageBox::information(this,"error",msg,0);
        return;
    }
    if(!prompt)
        return;

    QMessageBox::information(this,"DSI Studio","FIB file created.",0);
    if(method_id == 6)
        ((MainWindow*)parent())->addSrc(msg);
    else
        ((MainWindow*)parent())->addFib(msg);
}


void reconstruction_window::on_erosion_clicked()
{
    tipl::morphology::erosion(handle->voxel.mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_dilation_clicked()
{
    tipl::morphology::dilation(handle->voxel.mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_defragment_clicked()
{
    tipl::morphology::defragment(handle->voxel.mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_smoothing_clicked()
{
    tipl::morphology::smoothing(handle->voxel.mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}
void reconstruction_window::on_negate_clicked()
{
    tipl::morphology::negate(handle->voxel.mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}


void reconstruction_window::on_thresholding_clicked()
{
    bool ok;
    int threshold = QInputDialog::getInt(this,"DSI Studio","Please assign the threshold",
                                         (int)tipl::segmentation::otsu_threshold(dwi),
                                         (int)*std::min_element(dwi.begin(),dwi.end()),
                                         (int)*std::max_element(dwi.begin(),dwi.end())+1,1,&ok);
    if (!ok)
        return;
    tipl::threshold(dwi,handle->voxel.mask,threshold);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_load_mask_clicked()
{
    QString filename = QFileDialog::getOpenFileName(
            this,
            "Open region",
            absolute_path,
            "Mask files (*.txt *.nii *nii.gz *.hdr);;All files (*)" );
    if(filename.isEmpty())
        return;
    ROIRegion region(std::make_shared<fib_data>(dwi.geometry(),handle->voxel.vs));
    region.LoadFromFile(filename.toLocal8Bit().begin());
    region.SaveToBuffer(handle->voxel.mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}


void reconstruction_window::on_save_mask_clicked()
{
    QString filename = QFileDialog::getSaveFileName(
            this,
            "Save region",
            absolute_path+"/mask.txt",
            "Text files (*.txt);;Nifti file(*nii.gz *.nii);;All files (*)" );
    if(filename.isEmpty())
        return;
    if(QFileInfo(filename.toLower()).completeSuffix() != "txt")
        filename = QFileInfo(filename).absolutePath() + "/" + QFileInfo(filename).baseName() + ".nii.gz";
    ROIRegion region(std::make_shared<fib_data>(dwi.geometry(),handle->voxel.vs));
    region.LoadFromBuffer(handle->voxel.mask);
    region.SaveToFile(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_doDTI_clicked()
{
    for(int index = 0;index < filenames.size();++index)
    {
        if(index)
        {
            begin_prog("load src");
            if(!load_src(index))
                return;
        }
        std::fill(handle->voxel.param.begin(),handle->voxel.param.end(),0.0);
        if(ui->DTI->isChecked())
            doReconstruction(1,index+1 == filenames.size());
        else
        if(ui->DSI->isChecked())
        {
            handle->voxel.param[0] = ui->hamming_filter->value();
            settings.setValue("rec_hamming_filter",handle->voxel.param[0]);
            doReconstruction(0,index+1 == filenames.size());
        }
        else
        if(ui->QBI->isChecked())
        {
            handle->voxel.param[0] = ui->regularization_param->value();
            handle->voxel.param[1] = ui->SHOrder->value();
            settings.setValue("rec_qbi_reg",handle->voxel.param[0]);
            settings.setValue("rec_qbi_sh_order",handle->voxel.param[1]);
            doReconstruction(3,index+1 == filenames.size());
        }
        else
        if(ui->DDI->isChecked())
        {
            if(!handle->study_src.get())
            {
                QMessageBox::information(this,"Error","Please assign study SRC file",0);
                return;
            }
            handle->voxel.param[0] = ui->diffusion_sampling->value();
            settings.setValue("rec_gqi_sampling",ui->diffusion_sampling->value());
            doReconstruction(8,index+1 == filenames.size());
        }
        if(ui->GQI->isChecked() || ui->QSDR->isChecked())
        {
            handle->voxel.param[0] = ui->diffusion_sampling->value();
            if(handle->voxel.param[0] == 0.0)
                handle->voxel.param[1] = ui->diffusion_time->value();
            settings.setValue("rec_gqi_sampling",ui->diffusion_sampling->value());
            if(ui->QSDR->isChecked())
                doReconstruction(7,index+1 == filenames.size());
            else
                doReconstruction(4,index+1 == filenames.size());
        }
        if(prog_aborted())
            break;
    }
}

void reconstruction_window::on_DTI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);
    //ui->ODFSharpening->setVisible(!checked);
    ui->DSIOption_2->setVisible(!checked);
    ui->QBIOption_2->setVisible(!checked);
    ui->GQIOption_2->setVisible(!checked);
    ui->DDIOption->setVisible(!checked);

    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(!checked);
    ui->output_mapping->setVisible(!checked);
    ui->output_jacobian->setVisible(!checked);
    ui->output_tensor->setVisible(checked);
    ui->output_diffusivity->setVisible(!checked);

    ui->RecordODF->setVisible(!checked);
    ui->rdi->setVisible(!checked);



}

void reconstruction_window::on_DSI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);
    //ui->ODFSharpening->setVisible(checked);
    ui->DSIOption_2->setVisible(checked);
    ui->QBIOption_2->setVisible(!checked);
    ui->GQIOption_2->setVisible(!checked);
    ui->DDIOption->setVisible(!checked);

    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(checked);

    ui->output_mapping->setVisible(!checked);
    ui->output_jacobian->setVisible(!checked);
    ui->output_tensor->setVisible(!checked);
    ui->output_diffusivity->setVisible(checked);

    ui->RecordODF->setVisible(checked);
    ui->rdi->setVisible(!checked);

}

void reconstruction_window::on_QBI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);
    //ui->ODFSharpening->setVisible(checked);
    ui->DSIOption_2->setVisible(!checked);
    ui->QBIOption_2->setVisible(checked);
    ui->GQIOption_2->setVisible(!checked);
    ui->DDIOption->setVisible(!checked);

    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(checked);

    ui->output_mapping->setVisible(!checked);
    ui->output_jacobian->setVisible(!checked);
    ui->output_tensor->setVisible(!checked);
    ui->output_diffusivity->setVisible(checked);

    ui->RecordODF->setVisible(checked);
    ui->rdi->setVisible(!checked);

}

void reconstruction_window::on_GQI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);


    ui->DSIOption_2->setVisible(!checked);
    ui->QBIOption_2->setVisible(!checked);
    ui->GQIOption_2->setVisible(checked);
    ui->DDIOption->setVisible(!checked);

    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(checked);

    ui->output_mapping->setVisible(!checked);
    ui->output_jacobian->setVisible(!checked);
    ui->output_tensor->setVisible(!checked);
    ui->output_diffusivity->setVisible(checked);

    ui->RecordODF->setVisible(checked);

    ui->rdi->setVisible(checked);
    if(checked)
        ui->rdi->setChecked(true);
    ui->csf_calibration->setVisible(handle->is_human_data());
}
void reconstruction_window::on_DDI_toggled(bool checked)
{
    on_GQI_toggled(checked);
    ui->rdi->setVisible(false);
    ui->DDIOption->setVisible(checked);
    ui->csf_calibration->setVisible(false);

}

void reconstruction_window::on_QSDR_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(checked);
    //ui->ODFSharpening->setVisible(checked);
    ui->DSIOption_2->setVisible(!checked);
    ui->QBIOption_2->setVisible(!checked);
    ui->GQIOption_2->setVisible(checked);
    ui->DDIOption->setVisible(!checked);


    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(checked);

    ui->output_mapping->setVisible(checked);
    ui->output_jacobian->setVisible(checked);
    ui->output_tensor->setVisible(!checked);
    ui->output_diffusivity->setVisible(checked);

    ui->RecordODF->setVisible(checked);
    ui->rdi->setVisible(checked);
    if(checked)
        ui->rdi->setChecked(true);

    ui->csf_calibration->setVisible(false);
}


void reconstruction_window::on_remove_background_clicked()
{
    for(int index = 0;index < handle->voxel.mask.size();++index)
        if(handle->voxel.mask[index] == 0)
            dwi[index] = 0;

    for(int index = 0;index < handle->src_dwi_data.size();++index)
    {
        unsigned short* buf = (unsigned short*)handle->src_dwi_data[index];
        for(int i = 0;i < handle->voxel.mask.size();++i)
            if(handle->voxel.mask[i] == 0)
                buf[i] = 0;
    }
    on_SlicePos_valueChanged(ui->SlicePos->value());
}


void reconstruction_window::on_zoom_in_clicked()
{
    source_ratio *= 1.1f;
    on_b_table_itemSelectionChanged();
}

void reconstruction_window::on_zoom_out_clicked()
{
    source_ratio *= 0.9f;
    on_b_table_itemSelectionChanged();
}

extern fa_template fa_template_imp;
void reconstruction_window::on_manual_reg_clicked()
{
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
            dwi,handle->voxel.vs,
            fa_template_imp.I,fa_template_imp.vs,
            tipl::reg::affine,tipl::reg::cost_type::corr));
    if(manual->exec() == QDialog::Accepted)
        handle->voxel.qsdr_trans = manual->T;
}

void reconstruction_window::on_odf_sharpening_currentIndexChanged(int)
{
    ui->xyz_widget->setVisible(ui->odf_sharpening->currentIndex() > 0);
    ui->decom_panel->setVisible(ui->odf_sharpening->currentIndex() == 2);
    ui->decon_param->setVisible(ui->odf_sharpening->currentIndex() == 1);
    on_RFSelection_currentIndexChanged(0);
}

void reconstruction_window::on_RFSelection_currentIndexChanged(int)
{
    ui->ODFSelection->setVisible(ui->RFSelection->currentIndex() > 0);
}

void reconstruction_window::on_AdvancedOptions_clicked()
{
    if(ui->AdvancedOptions->text() == "Advanced Options >>")
    {
        ui->AdvancedWidget->setVisible(true);
        ui->AdvancedOptions->setText("Advanced Options <<");
    }
    else
    {
        ui->AdvancedWidget->setVisible(false);
        ui->AdvancedOptions->setText("Advanced Options >>");
    }
}


void reconstruction_window::on_actionSave_4D_nifti_triggered()
{
    if(filenames.size() > 1)
    {
        for(int index = 0;check_prog(index,filenames.size());++index)
        {
            ImageModel model;
            if (!model.load_from_file(filenames[index].toLocal8Bit().begin()))
            {
                QMessageBox::information(this,"error",QString("Cannot open ") +
                    filenames[index] + " : " +handle->error_msg.c_str(),0);
                check_prog(0,0);
                return;
            }
            model.save_to_nii((filenames[index]+".nii.gz").toLocal8Bit().begin());
        }
        return;
    }
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save image as...",
                            filenames[0] + ".nii.gz",
                                "All files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_to_nii(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_b0_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save image as...",
                            filenames[0] + ".b0.nii.gz",
                                "All files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_b0_to_nii(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_b_table_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save b table as...",
                            QFileInfo(filenames[0]).absolutePath() + "/b_table.txt",
                                "Text files (*.txt)" );
    if ( filename.isEmpty() )
        return;
    handle->save_b_table(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_bvals_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save b table as...",
                                QFileInfo(filenames[0]).absolutePath() + "/bvals",
                                "Text files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_bval(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_bvecs_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save b table as...",
                                QFileInfo(filenames[0]).absolutePath() + "/bvecs",
                                "Text files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_bvec(filename.toLocal8Bit().begin());
}

void reconstruction_window::update_image(void)
{
    dwi.resize(handle->voxel.dim);
    float min = tipl::minimum(handle->dwi_sum);
    float range = tipl::maximum(handle->dwi_sum)-min;
    float r = range > 0.0 ? 255.9f/range:1.0f;
    for(unsigned int index = 0;index < dwi.size();++index)
        dwi[index] = (handle->dwi_sum[index]-min)*r;
    load_b_table();
}

void reconstruction_window::on_actionFlip_x_triggered()
{
    handle->flip_dwi(0);
    update_image();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_actionFlip_y_triggered()
{
    handle->flip_dwi(1);
    update_image();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_actionFlip_z_triggered()
{
    handle->flip_dwi(2);
    update_image();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_actionFlip_xy_triggered()
{
    begin_prog("rotating");
    handle->flip_dwi(3);
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}
void reconstruction_window::on_actionFlip_yz_triggered()
{
    begin_prog("rotating");
    handle->flip_dwi(4);
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}
void reconstruction_window::on_actionFlip_xz_triggered()
{
    begin_prog("rotating");
    handle->flip_dwi(5);
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}



bool load_image_from_files(QStringList filenames,tipl::image<float,3>& ref,tipl::vector<3>& vs);
void reconstruction_window::on_actionRotate_triggered()
{
    QStringList filenames = QFileDialog::getOpenFileNames(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz *.dcm);;All files (*)" );
    if( filenames.isEmpty())
        return;

    tipl::image<float,3> ref;
    tipl::vector<3> vs;
    if(!load_image_from_files(filenames,ref,vs))
        return;
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
                                                                dwi,handle->voxel.vs,ref,vs,
                                                                tipl::reg::rigid_body,
                                                                tipl::reg::cost_type::mutual_info));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;

    begin_prog("rotating");
    tipl::image<float,3> ref2(ref);
    float m = tipl::median(ref2.begin(),ref2.end());
    tipl::multiply_constant_mt(ref,0.5f/m);
    handle->rotate(ref,manual->iT);
    handle->voxel.vs = vs;
    handle->voxel.report += " The diffusion images were rotated and scaled to the space of ";
    handle->voxel.report += QFileInfo(filenames[0]).baseName().toStdString();
    handle->voxel.report += ". The b-table was also rotated accordingly.";
    ui->report->setText(handle->voxel.report.c_str());
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());

}


void reconstruction_window::on_delete_2_clicked()
{
    if(handle->src_dwi_data.size() == 1)
        return;
    unsigned int index = ui->b_table->currentRow();
    ui->b_table->removeRow(index);
    handle->remove(index);

}

void reconstruction_window::on_actionTrim_image_triggered()
{
    begin_prog("trimming");
    handle->trim();
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_diffusion_sampling_valueChanged(double arg1)
{
    if(arg1 == 0.0)
        ui->gqi_spectral->show();
    else
        ui->gqi_spectral->hide();
}

void reconstruction_window::on_SlicePos_valueChanged(int position)
{
    if (!dwi.size())
        return;
    buffer.resize(tipl::geometry<2>(dwi.width(),dwi.height()));
    unsigned int offset = position*buffer.size();
    std::copy(dwi.begin() + offset,dwi.begin()+ offset + buffer.size(),buffer.begin());

    unsigned char* slice_image_ptr = &*dwi.begin() + buffer.size()* position;
    unsigned char* slice_mask = &*handle->voxel.mask.begin() + buffer.size()* position;

    tipl::color_image buffer2(tipl::geometry<2>(dwi.width()*2,dwi.height()));
    tipl::draw(buffer,buffer2,tipl::vector<2,int>());
    for (unsigned int index = 0; index < buffer.size(); ++index)
    {
        unsigned char value = slice_image_ptr[index];
        if (slice_mask[index])
            buffer[index] = tipl::rgb(255, value, value);
        else
            buffer[index] = tipl::rgb(value, value, value);
    }
    tipl::draw(buffer,buffer2,tipl::vector<2,int>(dwi.width(),0));
    buffer2.swap(buffer);
    double ratio = std::max(1.0,
        std::min(((double)ui->graphicsView->width()-5)/(double)buffer.width(),
                 ((double)ui->graphicsView->height()-5)/(double)buffer.height()));
    slice_image = QImage((unsigned char*)&*buffer.begin(),buffer.width(),buffer.height(),QImage::Format_RGB32).
                    scaled(buffer.width()*ratio,buffer.height()*ratio);
    show_view(scene,slice_image);
}

void rec_motion_correction(ImageModel* handle)
{
    begin_prog("correcting");
    tipl::par_for2(handle->src_bvalues.size(),[&](int i,int id)
    {
        if(i == 0 || prog_aborted())
            return;
        if(id == 0)
            check_prog(i*99/handle->src_bvalues.size(),100);
        tipl::transformation_matrix<double> arg;
        bool terminated = false;
        tipl::reg::two_way_linear_mr(
                                  tipl::make_image(handle->src_dwi_data[0],handle->voxel.dim),
                                  handle->voxel.vs,
                                  tipl::make_image(handle->src_dwi_data[i],handle->voxel.dim),handle->voxel.vs,
                                  arg,tipl::reg::affine,tipl::reg::correlation(),terminated);
        handle->rotate_one_dwi(i,arg);
    });
    check_prog(1,1);

}

void reconstruction_window::on_motion_correction_clicked()
{
    rec_motion_correction(handle.get());
    if(!prog_aborted())
    {
        handle->calculate_dwi_sum();
        handle->voxel.calculate_mask(handle->dwi_sum);
        update_image();
    }
}

void reconstruction_window::on_scheme_balance_toggled(bool checked)
{
    if(checked)
        ui->half_sphere->setChecked(false);
}



void reconstruction_window::on_half_sphere_toggled(bool checked)
{
    if(checked)
        ui->scheme_balance->setChecked(false);
}

bool add_other_image(ImageModel* handle,QString name,QString filename,bool full_auto)
{
    tipl::image<float,3> ref;
    tipl::vector<3> vs;
    gz_nifti in;
    if(!in.load_from_file(filename.toLocal8Bit().begin()) || !in.toLPS(ref))
    {
        if(full_auto)
            std::cout << "Not a valid nifti file:" << filename.toStdString() << std::endl;
        else
            QMessageBox::information(0,"Error","Not a valid nifti file",0);
        return false;
    }
    tipl::transformation_matrix<double> affine;
    bool has_registered = false;
    for(unsigned int index = 0;index < handle->voxel.other_image.size();++index)
        if(ref.geometry() == handle->voxel.other_image[index].geometry())
        {
            affine = handle->voxel.other_image_affine[index];
            has_registered = true;
        }
    if(!has_registered && ref.geometry() != handle->voxel.dim)
    {
        in.get_voxel_size(vs.begin());
        if(full_auto)
        {
            std::cout << "add " << filename.toStdString() << " as " << name.toStdString() << std::endl;
            tipl::image<float,3> from(handle->dwi_sum),to(ref);
            tipl::normalize(from,1.0);
            tipl::normalize(to,1.0);
            bool terminated = false;
            tipl::affine_transform<float> arg;
            tipl::reg::linear_mr(from,handle->voxel.vs,to,vs,arg,tipl::reg::rigid_body,tipl::reg::mutual_information(),terminated,0.1);
            tipl::reg::linear_mr(from,handle->voxel.vs,to,vs,arg,tipl::reg::rigid_body,tipl::reg::mutual_information(),terminated,0.01);
            affine = tipl::transformation_matrix<float>(arg,handle->voxel.dim,handle->voxel.vs,to.geometry(),vs);
        }
        else
        {
            std::shared_ptr<manual_alignment> manual(new manual_alignment(0,
                        handle->dwi_sum,handle->voxel.vs,ref,vs,tipl::reg::rigid_body,tipl::reg::cost_type::mutual_info));
            manual->on_rerun_clicked();
            if(manual->exec() != QDialog::Accepted)
                return false;
            affine = manual->T;
        }

    }
    handle->voxel.other_image.push_back(ref);
    handle->voxel.other_image_name.push_back(name.toLocal8Bit().begin());
    handle->voxel.other_image_affine.push_back(affine);
    return true;
}

void reconstruction_window::on_add_t1t2_clicked()
{
    QString filename = QFileDialog::getOpenFileName(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz);;All files (*)" );
    if( filename.isEmpty())
        return;
    add_other_image(handle.get(),QFileInfo(filename).baseName(),filename,false);
}

void reconstruction_window::on_actionManual_Rotation_triggered()
{
    std::shared_ptr<manual_alignment> manual(
                new manual_alignment(this,dwi,handle->voxel.vs,dwi,handle->voxel.vs,tipl::reg::rigid_body,tipl::reg::cost_type::mutual_info));
    if(manual->exec() != QDialog::Accepted)
        return;
    begin_prog("rotating");
    handle->rotate(dwi,manual->iT);
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}



void reconstruction_window::on_actionReplace_b0_by_T2W_image_triggered()
{
    QString filename = QFileDialog::getOpenFileName(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz);;All files (*)" );
    if( filename.isEmpty())
        return;
    tipl::image<float,3> ref;
    tipl::vector<3> vs;
    gz_nifti in;
    if(!in.load_from_file(filename.toLocal8Bit().begin()) || !in.toLPS(ref))
    {
        QMessageBox::information(this,"Error","Not a valid nifti file",0);
        return;
    }
    in.get_voxel_size(vs.begin());
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,dwi,handle->voxel.vs,ref,vs,tipl::reg::rigid_body,tipl::reg::cost_type::corr));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;

    begin_prog("rotating");
    handle->rotate(ref,manual->iT);
    handle->voxel.vs = vs;
    tipl::pointer_image<unsigned short,3> I = tipl::make_image((unsigned short*)handle->src_dwi_data[0],handle->voxel.dim);
    ref *= (float)(*std::max_element(I.begin(),I.end()))/(*std::max_element(ref.begin(),ref.end()));
    std::copy(ref.begin(),ref.end(),I.begin());
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_actionCorrect_AP_PA_scans_triggered()
{
    QMessageBox::information(this,"DSI Studio","Please assign another SRC file with phase encoding flipped",0);
    QString filename = QFileDialog::getOpenFileName(
            this,"Open SRC file",absolute_path,
            "Images (*src.gz);;All files (*)" );
    if( filename.isEmpty())
        return;

    begin_prog("load src");
    ImageModel src2;
    if (!src2.load_from_file(filename.toLocal8Bit().begin()))
    {
        QMessageBox::information(this,"error",QString("Cannot open ") +
           filename + " : " +src2.error_msg.c_str(),0);
        check_prog(0,0);
        return;
    }
    check_prog(0,0);
    if(handle->voxel.dim != src2.voxel.dim)
    {
        QMessageBox::information(this,"error","The image dimension is different.",0);
        return;
    }

    handle->distortion_correction(src2);
    update_image();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}



void reconstruction_window::on_actionEnable_TEST_features_triggered()
{
    ui->DSI->setVisible(true);
    ui->QBI->setVisible(true);
    ui->DDI->setVisible(true);
    ui->ODFSharpening->setVisible(true);
    ui->ODFSharpening->setEnabled(true);
}

void reconstruction_window::on_actionImage_upsample_to_T1W_TESTING_triggered()
{
    QStringList filenames = QFileDialog::getOpenFileNames(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz *.dcm);;All files (*)" );
    if( filenames.isEmpty())
        return;

    tipl::image<float,3> ref;
    tipl::vector<3> vs;
    if(!load_image_from_files(filenames,ref,vs))
        return;
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
                                                                dwi,handle->voxel.vs,ref,vs,
                                                                tipl::reg::rigid_body,
                                                                tipl::reg::cost_type::mutual_info));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;

    begin_prog("rotating");
    tipl::image<float,3> ref2(ref);
    float m = tipl::median(ref2.begin(),ref2.end());
    tipl::multiply_constant_mt(ref,0.5f/m);
    handle->rotate(ref,manual->iT,true);
    handle->voxel.vs = vs;
    handle->voxel.report += " The diffusion images were rotated and scaled to the space of ";
    handle->voxel.report += QFileInfo(filenames[0]).baseName().toStdString();
    handle->voxel.report += ". The b-table was also rotated accordingly.";
    ui->report->setText(handle->voxel.report.c_str());
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}


void reconstruction_window::on_open_ddi_study_src_clicked()
{
    QString filename = QFileDialog::getOpenFileName(
            this,"Open Study SRC file",absolute_path,
            "Images (*src.gz);;All files (*)" );
    if( filename.isEmpty())
        return;
    std::shared_ptr<ImageModel> bl(new ImageModel);
    if(!bl->load_from_file(filename.toStdString().c_str()))
    {
        QMessageBox::information(this,"error",QString("Cannot open ") +
            filename + " : " +bl->error_msg.c_str(),0);
        return;
    }
    handle->study_src = bl;
    handle->voxel.study_name = QFileInfo(filename).baseName().toStdString();
    ui->ddi_file->setText(QFileInfo(filename).baseName());
}
