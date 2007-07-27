// Copyright (c) 2007, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// -----------------------------------------------------------------------------
//
// AEGISUB
//
// Website: http://aegisub.cellosoft.com
// Contact: mailto:zeratul@cellosoft.com
//


///////////
// Headers
#include <wx/wxprec.h>
#include "dialog_detached_video.h"
#include "video_box.h"
#include "video_context.h"
#include "video_display.h"
#include "frame_main.h"


///////////////
// Constructor
DialogDetachedVideo::DialogDetachedVideo(FrameMain *par)
//: wxFrame(par,-1,_("Detached Video"))
: wxDialog(par,-1,_("Detached Video"),wxDefaultPosition,wxSize(400,300),wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX | wxMINIMIZE_BOX | wxWANTS_CHARS)
{
	// Set parent
	parent = par;

	// Set a background panel
	wxPanel *panel = new wxPanel(this,-1,wxDefaultPosition,wxDefaultSize,wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
	
	// Video area;
	videoBox = new VideoBox(panel);
	videoBox->videoDisplay->freeSize = true;

	// Set sizer
	wxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
	mainSizer->Add(videoBox,1,wxEXPAND | wxALL,5);
	panel->SetSizer(mainSizer);
	mainSizer->SetSizeHints(this);

	// Update
	parent->SetDisplayMode(0,-1);
}


/////////////
// Destructor
DialogDetachedVideo::~DialogDetachedVideo() {
	parent->detachedVideo = NULL;
	parent->SetDisplayMode(1,-1);
}


///////////////
// Event table
BEGIN_EVENT_TABLE(DialogDetachedVideo,wxDialog)
	EVT_KEY_DOWN(DialogDetachedVideo::OnKey)
END_EVENT_TABLE()


////////////
// Key down
void DialogDetachedVideo::OnKey(wxKeyEvent &event) {
	// Send to parent... except that it doesn't work
	event.Skip();
	GetParent()->AddPendingEvent(event);
}
