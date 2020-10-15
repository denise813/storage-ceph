import { Component, Input, OnChanges } from '@angular/core';

import { RgwBucketService } from '../../../shared/api/rgw-bucket.service';

@Component({
  selector: 'cd-rgw-bucket-details',
  templateUrl: './rgw-bucket-details.component.html',
  styleUrls: ['./rgw-bucket-details.component.scss']
})
export class RgwBucketDetailsComponent implements OnChanges {
  @Input()
  selection: any;

  constructor(private rgwBucketService: RgwBucketService) {}

  ngOnChanges() {
    if (this.selection) {
      this.rgwBucketService.get(this.selection.bid).subscribe((bucket: object) => {
        this.selection = bucket;
      });
    }
  }
}
